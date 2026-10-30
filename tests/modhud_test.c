/* modhud_test.c -- bundle-declared weapon ammo displays (modhud.c).
 *
 * Offline: the hud object grammar, conflicts across bundles, the tree read
 * (installed and legacy bundle folders, admitted bundles only, using the
 * owner's sp-pistol descriptor shape), and the call-site relay against an
 * owned synthetic caller. With build 13954591 executables as arguments: the
 * call site and predicate resolve uniquely at their recorded RVAs, the call
 * decodes to the predicate, its rel32 is 4-byte aligned, and the widget
 * register and decl field are the ones the handler reads. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <shlobj.h>
#include "modhud.h"
#include "signatures.h"
#include "host_image.h"

void backend_log(const char *line) { (void)line; }

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

static int parse_one(const char *json, const char *owner)
{
    int ok;
    sh_modhud_begin();
    ok = sh_modhud_parse(json, strlen(json), owner);
    sh_modhud_publish(ok);
    return ok;
}

static void test_grammar(void)
{
    static const char sp[] = "{\"weapons\": {\"weapon/zion/player/sp/pistol\": {\"ammo_display\": \"weapon\"},"
                             " \"weapon/zion/player/sp/pistol_secondary_charge_shot\": {\"ammo_display\": \"weapon\"}}}";
    CHECK(parse_one(sp, "local.sp-pistol") && sh_modhud_count() == 2);
    CHECK(sh_modhud_select("weapon/zion/player/sp/pistol", 1) == 0);
    CHECK(sh_modhud_select("weapon/zion/player/sp/pistol_secondary_charge_shot", 1) == 0);
    CHECK(sh_modhud_select("weapon/zion/player/mp/shotgun", 1) == 1);
    CHECK(sh_modhud_select(NULL, 1) == 1);
    CHECK(parse_one("{\"weapons\": {\"weapon/a\": {\"ammo_display\": \"engine\"}}}", "x") && sh_modhud_count() == 1);
    CHECK(sh_modhud_select("weapon/a", 1) == 1);
    CHECK(parse_one("{\"schema\": \"snapmap-plus.weapon-hud.v1\", \"weapons\": {\"weapon/a\": {\"ammo_display\": \"weapon\"}}}", "x"));
    CHECK(parse_one("{}", "x") && sh_modhud_count() == 0);
    /* Refusals clear the live table. */
    CHECK(parse_one(sp, "a") && sh_modhud_count() == 2);
    CHECK(!parse_one("{\"weapons\": {\"weapon/a\": {\"ammo_display\": \"magic\"}}}", "x") && sh_modhud_count() == 0);
    CHECK(!parse_one("{\"weapons\": {\"weapon/a\": {\"ammo_display\": \"weapon\", \"color\": \"red\"}}}", "x"));
    CHECK(!parse_one("{\"weapons\": {\"Weapon/A\": {\"ammo_display\": \"weapon\"}}}", "x"));
    CHECK(!parse_one("{\"weapons\": {\"weapon/../a\": {\"ammo_display\": \"weapon\"}}}", "x"));
    CHECK(!parse_one("{\"weapons\": {}, \"crosshair\": 1}", "x"));
    CHECK(!parse_one("{\"schema\": \"other\", \"weapons\": {}}", "x"));
    CHECK(!parse_one("[1]", "x"));
    /* Identical rules compose; different rules for one weapon refuse the table. */
    sh_modhud_begin();
    CHECK(sh_modhud_parse(sp, strlen(sp), "a") && sh_modhud_parse(sp, strlen(sp), "b"));
    sh_modhud_publish(1);
    CHECK(sh_modhud_count() == 2);
    sh_modhud_begin();
    CHECK(sh_modhud_parse(sp, strlen(sp), "a"));
    {
        static const char eng[] = "{\"weapons\": {\"weapon/zion/player/sp/pistol\": {\"ammo_display\": \"engine\"}}}";
        CHECK(!sh_modhud_parse(eng, strlen(eng), "b"));
    }
    sh_modhud_publish(0);
    CHECK(sh_modhud_count() == 0);
}

/* ------------------------------------------------------------ tree */

static char g_root[MAX_PATH];
static const char H_PISTOL[] = "7259ac447432f2a95e4d98e53710f6c8ebe6d122938c55c9b4326cd2a85a7907";
static const char H_OTHER[]  = "1111111111111111111111111111111111111111111111111111111111111111";
static const char H_GONE[]   = "2222222222222222222222222222222222222222222222222222222222222222";
static int admitted(const char *hash) { return !strcmp(hash, H_PISTOL) || !strcmp(hash, H_OTHER); }

static void put(const char *rel, const char *text)
{
    char path[MAX_PATH], dir[MAX_PATH], *slash;
    FILE *f;
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", g_root, rel);
    strcpy_s(dir, sizeof dir, path);
    slash = strrchr(dir, '\\');
    *slash = 0;
    SHCreateDirectoryExA(NULL, dir, NULL);
    f = fopen(path, "wb");
    fputs(text, f);
    fclose(f);
}

/* A built mod folder: the author block (with `author` keys such as "hud")
 * and the product block naming the bundle hash. */
static void bundle(const char *rel, const char *id, const char *hash, const char *author)
{
    char text[2048];
    _snprintf_s(text, sizeof text, _TRUNCATE,
        "{\"id\": \"%s\", \"name\": \"%s\", \"version\": \"1\", %s\n\n"
        "  \"snapmap-plus\": {\"format\": 1, \"build\": 1, \"bundle\": \"%s\", \"files\": []}}",
        id, id, author, hash);
    put(rel, text);
}

static void test_tree(void)
{
    char tmp[MAX_PATH];
    GetTempPathA(sizeof tmp, tmp);
    _snprintf_s(g_root, sizeof g_root, _TRUNCATE, "%ssh_modhud_test_%lu\\mods", tmp, GetCurrentProcessId());
    /* The owner's converted sp-pistol, as the migration wrote it: a delivered
     * package is an ordinary folder now. */
    bundle("local.sp-pistol\\mod.json", "local.sp-pistol", H_PISTOL,
           "\"description\": \"Enable campaign pistol firing\", \"hud\": {\"weapons\": "
           "{\"weapon/zion/player/sp/pistol\": {\"ammo_display\": \"weapon\"}, "
           "\"weapon/zion/player/sp/pistol_secondary_charge_shot\": {\"ammo_display\": \"weapon\"}}},");
    bundle("alex.other\\mod.json", "alex.other", H_OTHER, "");
    /* Not admitted: its conflicting rule is ignored. */
    bundle("alex.gone\\mod.json", "alex.gone", H_GONE,
           "\"hud\": {\"weapons\": {\"weapon/zion/player/sp/pistol\": {\"ammo_display\": \"engine\"}}},");
    /* A folder without mod.json is not a mod. */
    put("notes\\readme.txt", "x");
    CHECK(sh_modhud_load_tree(g_root, admitted) == 2);
    CHECK(sh_modhud_select("weapon/zion/player/sp/pistol", 1) == 0);
    /* An admitted bundle with a different rule for the pistol refuses the table. */
    bundle("alex.other\\mod.json", "alex.other", H_OTHER,
           "\"hud\": {\"weapons\": {\"weapon/zion/player/sp/pistol\": {\"ammo_display\": \"engine\"}}},");
    CHECK(sh_modhud_load_tree(g_root, admitted) == -1 && sh_modhud_count() == 0);
    CHECK(sh_modhud_select("weapon/zion/player/sp/pistol", 1) == 1);
    /* A malformed hud object refuses too. */
    bundle("alex.other\\mod.json", "alex.other", H_OTHER, "\"hud\": {\"weapons\": 3},");
    CHECK(sh_modhud_load_tree(g_root, admitted) == -1);
    bundle("alex.other\\mod.json", "alex.other", H_OTHER, "");
    CHECK(sh_modhud_load_tree(g_root, admitted) == 2);
    /* A missing folder is an empty table. */
    CHECK(sh_modhud_load_tree("C:\\nonexistent\\sh_modhud\\mods", admitted) == 0);
    {
        static const char *const files[] = { "local.sp-pistol\\mod.json",
            "alex.other\\mod.json", "alex.gone\\mod.json", "notes\\readme.txt", NULL };
        static const char *const dirs[] = { "local.sp-pistol", "alex.other", "alex.gone", "notes", NULL };
        char path[MAX_PATH];
        for (int i = 0; files[i]; i++) { _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", g_root, files[i]); DeleteFileA(path); }
        for (int i = 0; dirs[i]; i++) { _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", g_root, dirs[i]); RemoveDirectoryA(path); }
        RemoveDirectoryA(g_root);
        *strrchr(g_root, '\\') = 0;
        RemoveDirectoryA(g_root);
    }
}

/* ------------------------------------------------------------ images */

static uint8_t *map_pe_by_rva(const char *path)
{
    FILE *f = fopen(path, "rb");
    long fsz;
    uint8_t *file, *img;
    IMAGE_DOS_HEADER *dos;
    IMAGE_NT_HEADERS *nt;
    IMAGE_SECTION_HEADER *sec;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); fsz = ftell(f); fseek(f, 0, SEEK_SET);
    file = (uint8_t *)malloc(fsz);
    if (!file || fread(file, 1, fsz, f) != (size_t)fsz) { fclose(f); free(file); return NULL; }
    fclose(f);
    dos = (IMAGE_DOS_HEADER *)file;
    nt = (IMAGE_NT_HEADERS *)(file + dos->e_lfanew);
    img = (uint8_t *)calloc(1, nt->OptionalHeader.SizeOfImage);
    if (!img) { free(file); return NULL; }
    memcpy(img, file, nt->OptionalHeader.SizeOfHeaders);
    sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        uint32_t va = sec[i].VirtualAddress, rs = sec[i].SizeOfRawData, po = sec[i].PointerToRawData;
        if (rs && va + rs <= nt->OptionalHeader.SizeOfImage && po + rs <= (uint32_t)fsz) memcpy(img + va, file + po, rs);
    }
    free(file);
    return img;
}

static const sig_entry *entry_named(const char *name)
{
    for (size_t i = 0; BACKEND_ENGINE_SIGNATURES[i].name; i++)
        if (!strcmp(BACKEND_ENGINE_SIGNATURES[i].name, name)) return &BACKEND_ENGINE_SIGNATURES[i];
    return NULL;
}

static void test_image(const char *path)
{
    uint8_t *base = map_pe_by_rva(path);
    wchar_t wide[MAX_PATH];
    const sh_build_identity *id;
    const sig_entry *site_e = entry_named("WeaponHudModeCall"), *pred_e = entry_named("WeaponHudGameMode");
    sig_result site, pred;
    int32_t d;
    CHECK(base && site_e && pred_e);
    if (!base || !site_e || !pred_e) { free(base); return; }
    MultiByteToWideChar(CP_ACP, 0, path, -1, wide, MAX_PATH);
    id = sh_host_test_bind_image(base, wide);
    CHECK(id != NULL);
    if (!id) { free(base); return; }
    CHECK(sig_resolve_one(base, site_e, &site) == SIG_OK);
    CHECK(sig_resolve_one(base, pred_e, &pred) == SIG_OK);
    CHECK(site.rva == (id->vulkan ? site_e->rva_vk : site_e->rva_gl));
    CHECK(pred.rva == (id->vulkan ? pred_e->rva_vk : pred_e->rva_gl));
    memcpy(&d, base + site.rva + 1, sizeof d);
    CHECK(base[site.rva] == 0xE8 && site.rva + 5 + d == pred.rva);
    CHECK(((site.rva + 1) & 3u) == 0);
    /* The widget is RBX: MOV RAX,[RBX+0x1F8] right after the TEST/JZ. */
    CHECK(!memcmp(base + site.rva + 9, "\x48\x8B\x83\xF8\x01\x00\x00", 7));
    printf("%s %s: WeaponHudModeCall 0x%x -> WeaponHudGameMode 0x%x\n", g_fail ? "BAD" : "OK ",
           id->vulkan ? "Vulkan" : "OpenGL", site.rva, pred.rva);
    free(base);
}

int main(int argc, char **argv)
{
    test_grammar();
    test_tree();
    /* The relay reads the live table: weapon/test/unlimited is a weapon rule. */
    CHECK(parse_one("{\"weapons\": {\"weapon/test/unlimited\": {\"ammo_display\": \"weapon\"}}}", "test"));
    CHECK(sh_modhud_test_relay());
    for (int i = 1; i < argc; i++) test_image(argv[i]);
    printf("modhud_test: %s (%d image%s)\n", g_fail ? "FAIL" : "ok", argc - 1, argc == 2 ? "" : "s");
    return g_fail;
}
