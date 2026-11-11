/* modhud_test.c -- a mod's weapon ammo displays (modhud.c).
 *
 * Offline: the hud object grammar; two mods that disagree about one weapon
 * leave only that weapon to the game; the table built from the builds the
 * open map serves (one build per salt, builds the map does not use left out,
 * a mod whose hud entry does not read skipped alone); the table read again on
 * the HUD's next question after an index change or a Play entry, with no
 * restart; and the call-site relay against an owned synthetic caller. With
 * build 13954591 executables as arguments: the call site and predicate
 * resolve uniquely at their recorded RVAs, the call decodes to the predicate,
 * its rel32 is 4-byte aligned, and the widget register and decl field are the
 * ones the handler reads. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    sh_modhud_publish();
    return ok;
}

#define PISTOL "weapon/zion/player/sp/pistol"
#define CHARGE "weapon/zion/player/sp/pistol_secondary_charge_shot"
#define SHOTGUN "weapon/zion/player/mp/shotgun"

static void test_grammar(void)
{
    static const char sp[] = "{\"weapons\": {\"" PISTOL "\": {\"ammo_display\": \"weapon\"},"
                             " \"" CHARGE "\": {\"ammo_display\": \"weapon\"}}}";
    static const char eng[] = "{\"weapons\": {\"" PISTOL "\": {\"ammo_display\": \"engine\"}}}";
    CHECK(parse_one(sp, "Campaign pistol") && sh_modhud_count() == 2);
    CHECK(sh_modhud_select(PISTOL, 1) == 0);
    CHECK(sh_modhud_select(CHARGE, 1) == 0);
    CHECK(sh_modhud_select(SHOTGUN, 1) == 1);
    CHECK(sh_modhud_select(NULL, 1) == 1);
    CHECK(parse_one("{\"weapons\": {\"weapon/a\": {\"ammo_display\": \"engine\"}}}", "x") && sh_modhud_count() == 0);
    CHECK(sh_modhud_select("weapon/a", 1) == 1);
    CHECK(parse_one("{}", "x") && sh_modhud_count() == 0);
    /* Objects that do not read add nothing. */
    CHECK(!parse_one("{\"schema\": \"snapmap-plus.weapon-hud.v1\", \"weapons\": {\"weapon/a\": {\"ammo_display\": \"weapon\"}}}", "x"));
    CHECK(!parse_one("{\"weapons\": {\"weapon/a\": {\"ammo_display\": \"magic\"}}}", "x") && sh_modhud_count() == 0);
    CHECK(!parse_one("{\"weapons\": {\"weapon/a\": {\"ammo_display\": \"weapon\", \"color\": \"red\"}}}", "x"));
    CHECK(!parse_one("{\"weapons\": {\"Weapon/A\": {\"ammo_display\": \"weapon\"}}}", "x"));
    CHECK(!parse_one("{\"weapons\": {\"weapon/../a\": {\"ammo_display\": \"weapon\"}}}", "x"));
    CHECK(!parse_one("{\"weapons\": {}, \"crosshair\": 1}", "x"));
    CHECK(!parse_one("[1]", "x"));
    /* A mod's table is used whole or not at all: one bad member adds none of it. */
    sh_modhud_begin();
    CHECK(sh_modhud_parse(sp, strlen(sp), "a"));
    {
        static const char half[] = "{\"weapons\": {\"" SHOTGUN "\": {\"ammo_display\": \"weapon\"},"
                                   " \"weapon/b\": {\"ammo_display\": \"magic\"}}}";
        CHECK(!sh_modhud_parse(half, strlen(half), "b"));
    }
    sh_modhud_publish();
    CHECK(sh_modhud_count() == 2 && sh_modhud_select(SHOTGUN, 1) == 1 && sh_modhud_select(PISTOL, 1) == 0);
    /* Identical rules compose. */
    sh_modhud_begin();
    CHECK(sh_modhud_parse(sp, strlen(sp), "a") && sh_modhud_parse(sp, strlen(sp), "b"));
    sh_modhud_publish();
    CHECK(sh_modhud_count() == 2);
    /* Two mods that disagree about the pistol: the pistol keeps the game's
     * display, every other rule stays. */
    sh_modhud_begin();
    CHECK(sh_modhud_parse(sp, strlen(sp), "a"));
    CHECK(sh_modhud_parse(eng, strlen(eng), "b"));
    {
        static const char third[] = "{\"weapons\": {\"" PISTOL "\": {\"ammo_display\": \"weapon\"}}}";
        CHECK(sh_modhud_parse(third, strlen(third), "c"));   /* a third mod does not bring it back */
    }
    sh_modhud_publish();
    CHECK(sh_modhud_count() == 1);
    CHECK(sh_modhud_select(PISTOL, 1) == 1);
    CHECK(sh_modhud_select(CHARGE, 1) == 0);
}

/* ------------------------------------------------------------ the builds */

/* The serving index's builds: salt, build hash, place, author keys, and
 * whether the open map serves it. */
typedef struct fake_build { const char *salt, *hash; int place; const char *extra; int serves; } fake_build;
static fake_build g_builds[8];
static int g_nbuilds, g_each_fails, g_loads;

static int fake_each(sh_modserve_extra_fn fn, void *context)
{
    int i;
    if (g_each_fails) return -1;
    for (i = 0; i < g_nbuilds; i++) fn(context, g_builds[i].salt, g_builds[i].hash, g_builds[i].place, g_builds[i].extra);
    return g_nbuilds;
}
static int fake_serves(const char *hash)
{
    int i;
    for (i = 0; i < g_nbuilds; i++) if (!strcmp(g_builds[i].hash, hash)) return g_builds[i].serves;
    return 0;
}
static int fake_reload(void) { g_loads++; return sh_modhud_load(fake_each, fake_serves, NULL); }

static void build(const char *salt, const char *hash, int place, const char *extra, int serves)
{
    fake_build *b = &g_builds[g_nbuilds++];
    b->salt = salt; b->hash = hash; b->place = place; b->extra = extra; b->serves = serves;
}

#define H1 "1111111111111111111111111111111111111111111111111111111111111111"
#define H2 "2222222222222222222222222222222222222222222222222222222222222222"
#define H3 "3333333333333333333333333333333333333333333333333333333333333333"
#define H4 "4444444444444444444444444444444444444444444444444444444444444444"
#define HUD_WEAPON(w) "{\"hud\": {\"weapons\": {\"" w "\": {\"ammo_display\": \"weapon\"}}}}"
#define HUD_ENGINE(w) "{\"hud\": {\"weapons\": {\"" w "\": {\"ammo_display\": \"engine\"}}}}"

static void test_builds(void)
{
    /* The root build of a salt and a downloaded map's build of it differ:
     * only the one the open map serves counts. */
    g_nbuilds = 0;
    build("pistolsp", H1, 0, HUD_WEAPON(PISTOL), 1);
    build("pistolsp", H2, 1, HUD_ENGINE(PISTOL), 0);
    build("shotgunm", H3, 0, "{\"strings\": {}}", 1);
    build("unusedmd", H4, 0, HUD_WEAPON(SHOTGUN), 0);   /* the map does not use it */
    CHECK(sh_modhud_load(fake_each, fake_serves, NULL) == 1);
    CHECK(sh_modhud_select(PISTOL, 1) == 0 && sh_modhud_select(SHOTGUN, 1) == 1);
    /* The map now serves the place's build: its rule, not the root's. */
    g_builds[0].serves = 0;
    g_builds[1].serves = 1;
    CHECK(sh_modhud_load(fake_each, fake_serves, NULL) == 0);
    CHECK(sh_modhud_select(PISTOL, 1) == 1);
    /* Two builds of one salt both served (map scope off): the first is read. */
    g_builds[0].serves = 1;
    CHECK(sh_modhud_load(fake_each, fake_serves, NULL) == 1 && sh_modhud_select(PISTOL, 1) == 0);
    /* A mod whose hud entry does not read is skipped; the others apply. */
    g_builds[2].extra = "{\"hud\": {\"weapons\": 3}}";
    g_builds[3].serves = 1;
    CHECK(sh_modhud_load(fake_each, fake_serves, NULL) == 2 && sh_modhud_select(SHOTGUN, 1) == 0);
    /* Builds that cannot be visited leave the game's displays. */
    g_each_fails = 1;
    CHECK(sh_modhud_load(fake_each, fake_serves, NULL) == -1 && sh_modhud_count() == 0);
    g_each_fails = 0;
}

/* An index change (an install, a Refresh, a removal) or a Play entry marks the
 * table stale; the HUD's next question reads it again, with no restart. */
static void test_follows_index(void)
{
    g_nbuilds = 0;
    g_loads = 0;
    build("pistolsp", H1, 0, HUD_WEAPON(PISTOL), 1);
    sh_modhud_test_reload_with(fake_reload);
    sh_modhud_mark_stale();
    CHECK(sh_modhud_select(PISTOL, 1) == 0 && g_loads == 1);
    CHECK(sh_modhud_select(PISTOL, 1) == 0 && g_loads == 1);     /* not read again until something changed */
    /* The mod is removed. */
    g_nbuilds = 0;
    CHECK(sh_modhud_select(PISTOL, 1) == 0);                      /* nothing told the HUD yet */
    sh_modhud_mark_stale();
    CHECK(sh_modhud_select(PISTOL, 1) == 1 && g_loads == 2);
    /* A mod is installed mid-session. */
    build("shotgunm", H3, 0, HUD_WEAPON(SHOTGUN), 1);
    sh_modhud_mark_stale();
    CHECK(sh_modhud_select(SHOTGUN, 1) == 0 && g_loads == 3);
    sh_modhud_test_reload_with(NULL);
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

/* Any number of rules, of weapon names of any length. */
static void test_any_count(void)
{
    size_t cap = 1u << 20, at = 0;
    char *t = (char *)malloc(cap), name[600];
    int i;
    CHECK(t != NULL);
    if (!t) return;
    at += (size_t)_snprintf_s(t + at, cap - at, _TRUNCATE, "{\"weapons\": {");
    for (i = 0; i < 2000; i++)
        at += (size_t)_snprintf_s(t + at, cap - at, _TRUNCATE, "%s\"weapon/many/w%d\": {\"ammo_display\": \"weapon\"}",
                                  i ? ", " : "", i);
    memset(name, 'a', 500);
    name[500] = 0;
    at += (size_t)_snprintf_s(t + at, cap - at, _TRUNCATE, ", \"weapon/%s\": {\"ammo_display\": \"weapon\"}}}", name);
    CHECK(parse_one(t, "many") && sh_modhud_count() == 2001);
    CHECK(sh_modhud_select("weapon/many/w1999", 1) == 0);
    _snprintf_s(t, cap, _TRUNCATE, "weapon/%s", name);
    CHECK(sh_modhud_select(t, 1) == 0);
    free(t);
}

int main(int argc, char **argv)
{
    test_grammar();
    test_any_count();
    test_builds();
    test_follows_index();
    /* The relay reads the live table: weapon/test/unlimited is a weapon rule. */
    CHECK(parse_one("{\"weapons\": {\"weapon/test/unlimited\": {\"ammo_display\": \"weapon\"}}}", "test"));
    CHECK(sh_modhud_test_relay());
    for (int i = 1; i < argc; i++) test_image(argv[i]);
    printf("modhud_test: %s (%d image%s)\n", g_fail ? "FAIL" : "ok", argc - 1, argc == 2 ? "" : "s");
    return g_fail;
}
