/* weapon_hud_test.c -- package admission, composition and scoped native relay. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "weapon_hud.h"
#include "packages.h"
#include "package_runtime.h"
static int failures;
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); failures++; } } while (0)
void backend_log(const char *s) { (void)s; }
static const char valid[] = "{\"weapons\":{"
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
    CHECK(!parse("{\"weapons\":{"
        "\"weapon/test/unlimited\":{\"ammo_display\":\"engine\"}}}", "conflict"));
    CHECK(sh_weapon_hud_test_count() == 0);
    const char *bad[] = {
        "null", "[]",
        "{\"schema\":\"future\",\"weapons\":{}}",
        "{\"weapons\":{},\"script\":\"x\"}",
        "{\"weapons\":{\"a\":{\"ammo_display\":\"infinite\"}}}",
        "{\"weapons\":{\"a\":{\"ammo_display\":false}}}",
        "{\"weapons\":{\"a\":{\"ammo_display\":\"weapon\",\"damage\":4}}}",
        "{\"weapons\":{\"../a\":{\"ammo_display\":\"weapon\"}}}",
        "{\"weapons\":{\"a\\u0000b\":{\"ammo_display\":\"weapon\"}}}",
        "{\"weapons\":{\"a\":{\"ammo_display\":\"weapon\",\"ammo_display\":\"engine\"}}}",
        "{\"weapons\":{\"a\":{},\"a\":{}}}",
        "{\"weapons\":{\"a\":{\"ammo_display\":\"weapon\\u0000\"}}}"
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK(parse(valid, "first"));
        CHECK(!parse(bad[i], "bad"));
        CHECK(sh_weapon_hud_test_count() == 0);
        CHECK(sh_weapon_hud_select("weapon/test/unlimited", 1) == 1);
    }
    char rows[40000], row[160];
    strcpy_s(rows, sizeof rows, "{\"weapons\":{");
    for (int i = 0; i < 257; i++) {
        _snprintf_s(row, sizeof row, _TRUNCATE,
            "%s\"weapon/test/%d\":{\"ammo_display\":\"weapon\"}", i ? "," : "", i);
        strcat_s(rows, sizeof rows, row);
    }
    strcat_s(rows, sizeof rows, "}}");
    CHECK(!parse(rows, "too-many"));
    CHECK(sh_weapon_hud_test_count() == 0);

    char temp[MAX_PATH], root[MAX_PATH], over[MAX_PATH], pkg[MAX_PATH], marker[MAX_PATH];
    char descriptor[1024], directories[4][MAX_PATH], resource[MAX_PATH];
    char extra[MAX_PATH], extra_marker[MAX_PATH], policy_error[1024];
    static const char map[] = "{\"value\":\"used\",\"targetType\":\"idDeclEntityDef\"}";
    GetTempPathA(sizeof temp, temp);
    _snprintf_s(root, sizeof root, _TRUNCATE, "%sweapon-hud-test-%lu", temp, GetCurrentProcessId());
    _snprintf_s(over, sizeof over, _TRUNCATE, "%s/overrides", root);
    _snprintf_s(pkg, sizeof pkg, _TRUNCATE, "%s/example", over);
    _snprintf_s(marker, sizeof marker, _TRUNCATE, "%s/package.json", pkg);
    CHECK(CreateDirectoryA(root, NULL)); CHECK(CreateDirectoryA(over, NULL));
    CHECK(CreateDirectoryA(pkg, NULL));
    snprintf(directories[0], MAX_PATH, "%s/assets", pkg);
    snprintf(directories[1], MAX_PATH, "%s/assets/generated", pkg);
    snprintf(directories[2], MAX_PATH, "%s/assets/generated/decls", pkg);
    snprintf(directories[3], MAX_PATH, "%s/assets/generated/decls/entitydef", pkg);
    for (int i = 0; i < 4; i++) CHECK(CreateDirectoryA(directories[i], NULL));
    snprintf(resource, sizeof resource, "%s/used.decl", directories[3]);
    write_text(resource, "{ edit = { health = 10; } }");
    snprintf(descriptor, sizeof descriptor, "{\"id\":\"example\",\"name\":\"Example\",\"hud\":%s}", valid);
    write_text(marker, descriptor);
    sh_package_runtime_test_empty_catalog();
    CHECK(sh_package_runtime_refresh(root));
    CHECK(sh_package_runtime_select_map(NULL, 0, NULL, policy_error, sizeof policy_error));
    CHECK(sh_weapon_hud_reload(root));
    CHECK(sh_weapon_hud_test_count() == 0); /* Installed is not map-active. */
    CHECK(sh_package_runtime_select_map(map, sizeof map - 1, NULL, policy_error, sizeof policy_error));
    CHECK(sh_weapon_hud_reload(root));
    CHECK(sh_weapon_hud_test_count() == 1);
    CHECK(sh_weapon_hud_select("weapon/test/unlimited", 1) == 0);
    CHECK(!sh_package_runtime_select_map("{", 1, NULL, policy_error, sizeof policy_error));
    CHECK(sh_weapon_hud_reload(root));
    CHECK(sh_weapon_hud_test_count() == 1); /* Incomplete selection cannot publish. */

    /* Inline native fields contribute roots even without an explicit typed
     * wrapper. Runtime owns a copy after the preparation snapshot closes. */
    {
        sh_package_references references = {0};
        CHECK(sh_package_references_add(&references, "entitydef", "used"));
        CHECK(sh_package_runtime_select_map("{}", 2, &references, policy_error, sizeof policy_error));
        sh_package_references_free(&references);
        CHECK(sh_weapon_hud_reload(root) && sh_weapon_hud_test_count() == 1);
    }

    /* A new earlier folder changes every later owner index. Refresh must
     * resolve the map again and must not activate the unrelated package. */
    snprintf(extra, sizeof extra, "%s/aaa", over);
    snprintf(extra_marker, sizeof extra_marker, "%s/package.json", extra);
    CHECK(CreateDirectoryA(extra, NULL));
    write_text(extra_marker, "{\"id\":\"unrelated\",\"name\":\"Unrelated\",\"hud\":{\"weapons\":{"
        "\"weapon/test/other\":{\"ammo_display\":\"weapon\"}}}}");
    CHECK(sh_package_runtime_refresh(root));
    CHECK(sh_weapon_hud_reload(root));
    CHECK(sh_weapon_hud_test_count() == 1);
    CHECK(sh_weapon_hud_select("weapon/test/unlimited", 1) == 0);
    CHECK(sh_weapon_hud_select("weapon/test/other", 1) == 1);
    CHECK(sh_package_runtime_select_map("{}", 2, NULL, policy_error, sizeof policy_error));
    CHECK(sh_weapon_hud_reload(root));
    CHECK(sh_weapon_hud_test_count() == 0);
    CHECK(sh_weapon_hud_select("weapon/test/unlimited", 1) == 1);
    CHECK(sh_package_runtime_select_map(map, sizeof map - 1, NULL, policy_error, sizeof policy_error));
    CHECK(sh_weapon_hud_reload(root));
    CHECK(sh_weapon_hud_test_relay(root));
    sh_packages_test_find_api api = {FindFirstFileA, broken_next, FindClose, GetFileAttributesA};
    sh_packages_test_set_api(&api);
    CHECK(!sh_package_runtime_refresh(root));
    CHECK(sh_weapon_hud_reload(root));
    CHECK(sh_weapon_hud_test_count() == 1);
    sh_packages_test_reset_api();
    write_text(marker, "invalid");
    CHECK(!sh_package_runtime_refresh(root));
    CHECK(sh_weapon_hud_reload(root));
    CHECK(sh_weapon_hud_test_count() == 1);
    write_text(marker, descriptor);
    CHECK(sh_package_runtime_refresh(root));
    CHECK(sh_weapon_hud_reload(root));
    CHECK(DeleteFileA(resource));
    for (int i = 3; i >= 0; i--) CHECK(RemoveDirectoryA(directories[i]));
    CHECK(DeleteFileA(marker));
    CHECK(RemoveDirectoryA(pkg));
    CHECK(sh_package_runtime_refresh(root));
    CHECK(sh_weapon_hud_reload(root));
    CHECK(sh_weapon_hud_test_count() == 0);
    CHECK(DeleteFileA(extra_marker)); CHECK(RemoveDirectoryA(extra));
    CHECK(sh_package_runtime_select_map(NULL, 0, NULL, policy_error, sizeof policy_error));
    CHECK(RemoveDirectoryA(over)); CHECK(RemoveDirectoryA(root));
    printf("weapon_hud_test: %d failures\n", failures);
    return failures ? 1 : 0;
}
