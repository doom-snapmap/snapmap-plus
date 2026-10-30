/* modsound_test.c -- mod sound banks (modsound.c) and the builder's bank and
 * sound-decl rules (modbundle.c).
 *
 * Offline: the served-request to file-name rule, the wide-name match, the
 * s_loadBanks decode, the bank table (stock pointers, duplicates and the cap
 * refused), the Wwise open replacement against a real temporary file (the
 * AkFileDesc filled as the engine's default hook fills it, the handle reading
 * the bank's bytes, a wrong size or another name going to the original), the
 * submit pass (once per sound-system lifetime, nothing before Wwise is up,
 * again after LoadBanks), and the builder: a bank only at
 * sound/soundbanks/pc/<id>_<rest>.bnk without _sp, _snapmaps or initial,
 * version 113 with id FNV-1(stem), never a shadow; a mods/ soundShader is a
 * need of type sound, a stock one is not.
 * With build 13954591 executables as arguments: the four signatures resolve
 * at their recorded RVAs, the console function decodes to soundSystem and its
 * +0x140, and the read-only slots holding the open-by-name (two) and
 * LoadBanks (one) are the recorded vtable entries. Run via run-tests.ps1. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <shlobj.h>
#include "modsound.h"
#include "modbundle.h"
#include "signatures.h"
#include "host_image.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

/* ------------------------------------------------------------ pure rules */

static void test_names(void)
{
    char leaf[SH_MODSOUND_LEAF_MAX];
    CHECK(sh_modsound_leaf("sound/soundbanks/pc/p6probe_sfx.bnk", leaf, sizeof leaf) && !strcmp(leaf, "p6probe_sfx.bnk"));
    CHECK(sh_modsound_leaf("Sound\\SoundBanks\\PC\\P6probe_SFX.BNK", leaf, sizeof leaf) && !strcmp(leaf, "p6probe_sfx.bnk"));
    CHECK(!sh_modsound_leaf("sound/soundbanks/pc/english(us)/p6probe_vo.bnk", leaf, sizeof leaf));
    CHECK(!sh_modsound_leaf("sound/soundbanks/pc/.bnk", leaf, sizeof leaf));
    CHECK(!sh_modsound_leaf("sound/soundbanks/pc/p6probe_sfx.pck", leaf, sizeof leaf));
    CHECK(!sh_modsound_leaf("generated/decls/sound/mods/p6probe/p6probe_far.decl", leaf, sizeof leaf));
    CHECK(!sh_modsound_leaf("sound/soundbanks/pc/p6probe_sfx.bnk", leaf, 8));

    CHECK(sh_modsound_name_matches(L"p6probe_sfx.bnk", "p6probe_sfx.bnk"));
    CHECK(sh_modsound_name_matches(L"P6Probe_SFX.bnk", "p6probe_sfx.bnk"));
    CHECK(!sh_modsound_name_matches(L"p6probe_sfx.bnk2", "p6probe_sfx.bnk"));
    CHECK(!sh_modsound_name_matches(L"p6probe_sfx", "p6probe_sfx.bnk"));
    CHECK(!sh_modsound_name_matches(L"English(US)\\p6probe_sfx.bnk", "p6probe_sfx.bnk"));
    CHECK(!sh_modsound_name_matches(L"p6probe_sf\x00e9.bnk", "p6probe_sfx.bnk"));
    CHECK(!sh_modsound_name_matches(NULL, "p6probe_sfx.bnk"));
}

static void test_decode(void)
{
    /* The Vulkan body, relocated into a buffer: displacements are relative. */
    uint8_t c[40] = { 0x48, 0x83, 0xEC, 0x28, 0x48, 0x8D, 0x0D, 0x00, 0x01, 0x00, 0x00, 0xE8, 0, 0, 0, 0,
                      0x48, 0x8B, 0x0D, 0x34, 0x02, 0x00, 0x00, 0x48, 0x8B, 0x01, 0x48, 0x83, 0xC4, 0x28,
                      0x48, 0xFF, 0x60, 0x30 };
    uintptr_t ss = 0, slot = 0;
    /* LEA target = c+11+0x100; MOV target = c+23+0x234 = the LEA target + 0x140. */
    CHECK(sh_modsound_decode_cmd(c, &ss, &slot));
    CHECK(ss == (uintptr_t)c + 11 + 0x100 && slot == ss + 0x140);
    c[19] = 0x35;   /* the slot is no longer soundSystem+0x140 */
    CHECK(!sh_modsound_decode_cmd(c, &ss, &slot));
    c[19] = 0x34;
    c[33] = 0x28;   /* JMP [RAX+0x28]: another vtable slot */
    CHECK(!sh_modsound_decode_cmd(c, &ss, &slot));
    CHECK(!sh_modsound_decode_cmd(NULL, &ss, &slot));
}

static void test_table(void)
{
    sh_modsound_table t;
    wchar_t w[] = L"p6probe_sfx.bnk";
    int i;
    memset(&t, 0, sizeof t);
    CHECK(sh_modsound_table_add(&t, "sound/soundbanks/pc/p6probe_sfx.bnk", "C:\\m\\a.bnk", 82080, "p6probe", 0) == 1);
    CHECK(sh_modsound_table_add(&t, "generated/decls/sound/mods/p6probe/x.decl", "C:\\m\\x.decl", 10, "p6probe", 0) == 0);
    CHECK(sh_modsound_table_add(&t, "sound/soundbanks/pc/P6PROBE_SFX.bnk", "C:\\m\\b.bnk", 1, "other", 0) == -1);
    CHECK(sh_modsound_table_add(&t, "sound/soundbanks/pc/stocky_x.bnk", NULL, 1, "stocky", 1) == -1);
    CHECK(t.count == 1 && t.skipped == 2);
    CHECK(sh_modsound_lookup(&t, w, 0) == &t.banks[0]);
    CHECK(sh_modsound_lookup(&t, w, 1) == NULL);   /* write opens are never answered */
    CHECK(sh_modsound_lookup(&t, L"doom_ui.bnk", 0) == NULL);
    for (i = 1; i < SH_MODSOUND_MAX_BANKS; i++) {
        char r[64];
        _snprintf_s(r, sizeof r, _TRUNCATE, "sound/soundbanks/pc/p6probe_%d.bnk", i);
        CHECK(sh_modsound_table_add(&t, r, "C:\\m\\n.bnk", 1, "p6probe", 0) == 1);
    }
    CHECK(sh_modsound_table_add(&t, "sound/soundbanks/pc/p6probe_full.bnk", "C:\\m\\n.bnk", 1, "p6probe", 0) == -1);
    CHECK(t.count == SH_MODSOUND_MAX_BANKS);
    sh_modsound_table_free(&t);
    CHECK(t.count == 0);
}

/* ------------------------------------------------------------ the routes */

static int n_orig_open, n_orig_lb, n_submit;
static char last_submit[128];
static void *last_submit_hw;
static int submit_result = 1;
static int heap_depth, heap_refuse, lb_depth, submit_depth;
static int fake_enter(void) { if (heap_refuse) return 0; heap_depth++; return 1; }
static void fake_leave(void) { heap_depth--; }

static int32_t fake_open(void *self, const wchar_t *name, int32_t mode, const void *flags, uint8_t *sync,
                         sh_modsound_file_desc *desc)
{
    (void)self; (void)name; (void)mode; (void)flags; (void)sync; (void)desc;
    n_orig_open++;
    return 0x42;   /* AK_FileNotFound */
}
static void fake_loadbanks(void *hw) { (void)hw; n_orig_lb++; lb_depth = heap_depth; }
static int fake_submit(void *hw, const char *name)
{
    n_submit++;
    submit_depth = heap_depth;
    last_submit_hw = hw;
    strcpy_s(last_submit, sizeof last_submit, name);
    return submit_result;
}

static void test_routes(void)
{
    char tmp[MAX_PATH], path[MAX_PATH];
    uint8_t bytes[300], back[300], sync = 0;
    uint8_t resolver[0x640];
    uint8_t hardware[0x20];
    sh_modsound_table t;
    sh_modsound_file_desc d;
    DWORD got = 0;
    FILE *f;
    int i;
    for (i = 0; i < (int)sizeof bytes; i++) bytes[i] = (uint8_t)(i * 7);
    GetTempPathA(sizeof tmp, tmp);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%ssh_modsound_test_%lu.bnk", tmp, GetCurrentProcessId());
    f = fopen(path, "wb");
    fwrite(bytes, 1, sizeof bytes, f);
    fclose(f);
    memset(resolver, 0, sizeof resolver);
    *(uint32_t *)(resolver + SH_MODSOUND_DEVICE_OFF) = 0x1234;
    memset(hardware, 0, sizeof hardware);

    memset(&t, 0, sizeof t);
    CHECK(sh_modsound_table_add(&t, "sound/soundbanks/pc/p6probe_sfx.bnk", path, sizeof bytes, "p6probe", 0) == 1);
    sh_modsound_test_bind(fake_open, fake_loadbanks, fake_submit, hardware, &t);
    sh_modsound_test_heap(fake_enter, fake_leave);

    /* The bank's name: the bundle file, as the default hook would open it. */
    memset(&d, 0xCC, sizeof d);
    CHECK(sh_modsound_test_open(resolver, L"P6PROBE_SFX.bnk", 0, NULL, &sync, &d) == 1);
    CHECK(n_orig_open == 0 && sync == 1);
    CHECK(d.file_size == sizeof bytes && d.sector == 0 && d.custom_param_size == 0 && d.custom_param == NULL &&
          d.device_id == 0x1234 && d.reserved == 0 && d.handle && d.handle != INVALID_HANDLE_VALUE);
    if (d.handle && d.handle != INVALID_HANDLE_VALUE) {
        CHECK(ReadFile((HANDLE)d.handle, back, sizeof back, &got, NULL) && got == sizeof back && !memcmp(back, bytes, sizeof back));
        CHECK(CloseHandle((HANDLE)d.handle));
    }
    /* Other names, a write open and a missing descriptor go to the engine. */
    CHECK(sh_modsound_test_open(resolver, L"doom_ui.bnk", 0, NULL, &sync, &d) == 0x42 && n_orig_open == 1);
    CHECK(sh_modsound_test_open(resolver, L"p6probe_sfx.bnk", 1, NULL, &sync, &d) == 0x42 && n_orig_open == 2);
    CHECK(sh_modsound_test_open(resolver, L"p6probe_sfx.bnk", 0, NULL, &sync, NULL) == 0x42 && n_orig_open == 3);

    /* A pass before Wwise is up submits nothing. */
    CHECK(sh_modsound_test_pass() == 0 && n_submit == 0);
    hardware[8] = 2;   /* Init's "Wwise initialised" bit */
    CHECK(sh_modsound_test_pass() == 1 && n_submit == 1 && !strcmp(last_submit, "p6probe_sfx.bnk") &&
          last_submit_hw == hardware);
    /* Once per sound-system lifetime. */
    CHECK(sh_modsound_test_pass() == 0 && n_submit == 1);
    CHECK(submit_depth == 1 && heap_depth == 0);
    /* s_loadBanks / s_restart: the engine's LoadBanks inside process heap 0
     * (the stream packages it loads must outlive the open map), then every
     * bank again, each submit in its own scope. */
    sh_modsound_test_loadbanks(hardware);
    CHECK(n_orig_lb == 1 && n_submit == 2);
    CHECK(lb_depth == 1 && submit_depth == 1 && heap_depth == 0);
    /* A scope that cannot be entered still runs the engine's LoadBanks
     * (s_restart must rebuild Wwise) but submits nothing outside heap 0. */
    heap_refuse = 1;
    sh_modsound_test_loadbanks(hardware);
    CHECK(n_orig_lb == 2 && lb_depth == 0 && n_submit == 2 && heap_depth == 0);
    heap_refuse = 0;
    CHECK(sh_modsound_test_pass() == 1 && n_submit == 3);
    CHECK(sh_modsound_test_pass() == 0 && n_submit == 3);
    /* A refused submit is not retried in the same lifetime. */
    submit_result = 2;
    sh_modsound_test_loadbanks(hardware);
    CHECK(n_orig_lb == 3 && n_submit == 4 && sh_modsound_test_pass() == 0);
    submit_result = 1;

    /* A rebuilt table keeps what was already submitted under the same name;
     * a new bank is submitted at the next pass. */
    CHECK(sh_modsound_table_add(&t, "sound/soundbanks/pc/p6probe_more.bnk", path, sizeof bytes, "p6probe", 0) == 1);
    sh_modsound_test_bind(fake_open, fake_loadbanks, fake_submit, hardware, &t);
    CHECK(sh_modsound_test_pass() == 1 && n_submit == 5 && !strcmp(last_submit, "p6probe_more.bnk"));

    /* The file no longer has the indexed size: the engine answers. */
    f = fopen(path, "ab");
    fputc(0, f);
    fclose(f);
    CHECK(sh_modsound_test_open(resolver, L"p6probe_sfx.bnk", 0, NULL, &sync, &d) == 0x42 && n_orig_open == 4);
    sh_modsound_table_free(&t);
    DeleteFileA(path);
}

/* ------------------------------------------------------------ builder */

static char g_dir[MAX_PATH];
static void put_bytes(const char *rel, const void *data, size_t n)
{
    char path[MAX_PATH], dir[MAX_PATH];
    FILE *f;
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", g_dir, rel);
    for (char *p = path; *p; p++) if (*p == '/') *p = '\\';
    strcpy_s(dir, sizeof dir, path);
    *strrchr(dir, '\\') = 0;
    SHCreateDirectoryExA(NULL, dir, NULL);
    f = fopen(path, "wb");
    fwrite(data, 1, n, f);
    fclose(f);
}
static void put(const char *rel, const char *text) { put_bytes(rel, text, strlen(text)); }
static void drop(const char *rel)
{
    char path[MAX_PATH];
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", g_dir, rel);
    for (char *p = path; *p; p++) if (*p == '/') *p = '\\';
    DeleteFileA(path);
}

/* A minimal bank: BKHD (version, id, two more fields) only. */
static void put_bank(const char *rel, const char *stem, uint32_t version, int wrong_id)
{
    uint8_t b[24];
    uint32_t size = 16, id = sh_mb_fnv1_lower(stem, strlen(stem)) + (wrong_id ? 1 : 0), z = 0;
    memcpy(b, "BKHD", 4);
    memcpy(b + 4, &size, 4);
    memcpy(b + 8, &version, 4);
    memcpy(b + 12, &id, 4);
    memcpy(b + 16, &z, 4);
    memcpy(b + 20, &z, 4);
    put_bytes(rel, b, sizeof b);
}

static const sh_mb_file *find_file(const sh_mb_bundle *b, const char *request)
{
    size_t i;
    for (i = 0; i < b->file_count; i++) if (!strcmp(b->files[i].request, request)) return &b->files[i];
    return NULL;
}

static int has_need(const sh_mb_file *f, const char *type, const char *name)
{
    size_t i;
    for (i = 0; f && i < f->need_count; i++)
        if (!strcmp(f->needs[i].type, type) && !_stricmp(f->needs[i].name, name)) return 1;
    return 0;
}

#define BANK "assets/sound/soundbanks/pc/p6probe_sfx.bnk"
#define SND  "assets/generated/decls/sound/mods/p6probe/p6probe_far.decl"
#define ENT  "assets/generated/decls/entitydef/mods/p6probe/speaker_far.decl"
#define DESC "{\"id\": \"p6probe\", \"version\": \"1.0.0\", \"name\": \"Sound probe\"}"

static void build_expect(int ok, const char *cause_part)
{
    static const sh_mb_resolver resolver = { NULL, NULL, NULL, NULL };
    sh_mb_bundle b;
    int r = sh_mb_build(g_dir, &resolver, &b);
    CHECK(r == ok);
    if (!ok) {
        CHECK(strstr(b.first_cause, cause_part) != NULL);
        if (!strstr(b.first_cause, cause_part)) printf("  cause: %s\n", b.first_cause);
    } else if (!r) printf("  build: %s\n", b.first_cause);
    sh_mb_free(&b);
}

static void test_builder(void)
{
    static const sh_mb_resolver resolver = { NULL, NULL, NULL, NULL };
    char tmp[MAX_PATH], cause[SH_MB_CAUSE_MAX], t[64], n[512];
    const sh_mb_file *f;
    sh_mb_bundle b;
    uint8_t head[16];

    /* FNV-1 is Wwise's GetIDFromString: the shipped banks' ids. */
    CHECK(sh_mb_fnv1_lower("doom_initial", 12) == 2191464771u);
    CHECK(sh_mb_fnv1_lower("DOOM_UI", 7) == 3132051923u);
    CHECK(sh_mb_fnv1_lower("p6probe_sfx", 11) == 3604064235u);

    CHECK(sh_mb_extension_allowed("sound/soundbanks/pc/p6probe_sfx.bnk"));
    CHECK(sh_mb_logical_name("sound/soundbanks/pc/p6probe_sfx.bnk", t, sizeof t, n, sizeof n) &&
          !strcmp(t, "soundBank") && !strcmp(n, "sound/soundbanks/pc/p6probe_sfx.bnk"));
    CHECK(sh_mb_logical_name("generated/decls/sound/mods/p6probe/p6probe_far.decl", t, sizeof t, n, sizeof n) &&
          !strcmp(t, "sound") && !strcmp(n, "mods/p6probe/p6probe_far"));

    CHECK(sh_mb_bank_rule("sound/soundbanks/pc/p6probe_sfx.bnk", "p6probe", cause, sizeof cause));
    CHECK(sh_mb_bank_rule("sound/soundbanks/pc/alex_pink_imp_growls.bnk", "alex.pink-imp", cause, sizeof cause));
    CHECK(!sh_mb_bank_rule("sound/soundbanks/pc/alex.pink-imp_growls.bnk", "alex.pink-imp", cause, sizeof cause) &&
          strstr(cause, "letters, digits"));
    CHECK(!sh_mb_bank_rule("sound/soundbanks/pc/doom_ui.bnk", "p6probe", cause, sizeof cause) && strstr(cause, "p6probe_"));
    CHECK(!sh_mb_bank_rule("sound/soundbanks/pc/p6probe_.bnk", "p6probe", cause, sizeof cause));
    CHECK(!sh_mb_bank_rule("sound/soundbanks/pc/p6probe_spider.bnk", "p6probe", cause, sizeof cause) && strstr(cause, "_sp"));
    CHECK(!sh_mb_bank_rule("sound/soundbanks/pc/p6probe_x_snapmaps.bnk", "p6probe", cause, sizeof cause));
    CHECK(!sh_mb_bank_rule("sound/soundbanks/pc/p6probe_initial.bnk", "p6probe", cause, sizeof cause));
    CHECK(!sh_mb_bank_rule("sound/soundbanks/pc/english(us)/p6probe_vo.bnk", "p6probe", cause, sizeof cause) &&
          strstr(cause, "sound/soundbanks/pc/<stem>.bnk"));
    CHECK(!sh_mb_bank_rule("sound/mods/p6probe/p6probe_sfx.bnk", "p6probe", cause, sizeof cause));

    memcpy(head, "BKHD", 4);
    { uint32_t v = 16; memcpy(head + 4, &v, 4); v = 0x71; memcpy(head + 8, &v, 4); v = 3604064235u; memcpy(head + 12, &v, 4); }
    CHECK(!sh_mb_bank_header_ok(head, 16, "p6probe_sfx", cause, sizeof cause) && strstr(cause, "truncated"));
    {
        uint8_t full[24];
        memcpy(full, head, 16);
        memset(full + 16, 0, 8);
        CHECK(sh_mb_bank_header_ok(full, sizeof full, "p6probe_sfx", cause, sizeof cause));
        CHECK(!sh_mb_bank_header_ok(full, sizeof full, "p6probe_sfx2", cause, sizeof cause) && strstr(cause, "FNV-1"));
        full[8] = 0x78;
        CHECK(!sh_mb_bank_header_ok(full, sizeof full, "p6probe_sfx", cause, sizeof cause) && strstr(cause, "version 120"));
        full[0] = 'X';
        CHECK(!sh_mb_bank_header_ok(full, sizeof full, "p6probe_sfx", cause, sizeof cause) && strstr(cause, "BKHD"));
    }

    GetTempPathA(sizeof tmp, tmp);
    _snprintf_s(g_dir, sizeof g_dir, _TRUNCATE, "%ssh_modsound_test_%lu\\p6probe", tmp, GetCurrentProcessId());
    put("mod.json", DESC);
    put_bank(BANK, "p6probe_sfx", 0x71, 0);
    put(SND, "{\n\tinherit = \"default\";\n}\n");
    put(ENT, "{\n\tinherit = \"speaker\";\n\tedit = {\n\t\tsoundShader = \"mods/p6probe/p6probe_far\";\n"
             "\t\twaitForTrigger = false;\n\t}\n}\n");
    CHECK(sh_mb_build(g_dir, &resolver, &b));
    if (b.errors) printf("  build: %s\n", b.first_cause);
    f = find_file(&b, "sound/soundbanks/pc/p6probe_sfx.bnk");
    CHECK(f && !strcmp(f->ptype, "soundBank") && f->size == 24 && !f->stock);
    f = find_file(&b, "generated/decls/sound/mods/p6probe/p6probe_far.decl");
    CHECK(f && !strcmp(f->ptype, "sound") && !strcmp(f->pname, "mods/p6probe/p6probe_far"));
    f = find_file(&b, "generated/decls/entitydef/mods/p6probe/speaker_far.decl");
    CHECK(f && has_need(f, "sound", "mods/p6probe/p6probe_far"));
    sh_mb_free(&b);

    /* A stock sound name is not a need; a mods/ one nobody provides is an error. */
    put(ENT, "{\n\tinherit = \"speaker\";\n\tedit = {\n\t\tsoundShader = \"play_arcade_ui_17\";\n\t}\n}\n");
    CHECK(sh_mb_build(g_dir, &resolver, &b));
    f = find_file(&b, "generated/decls/entitydef/mods/p6probe/speaker_far.decl");
    CHECK(f && f->need_count == 0);
    sh_mb_free(&b);
    put(ENT, "{\n\tinherit = \"speaker\";\n\tedit = {\n\t\tsoundShader = \"mods/p6probe/missing\";\n\t}\n}\n");
    build_expect(0, "mods/p6probe/missing");
    put(ENT, "{\n\tinherit = \"speaker\";\n}\n");

    /* Bank refusals. */
    put_bank(BANK, "p6probe_sfx", 0x71, 1);
    build_expect(0, "FNV-1");
    put_bank(BANK, "p6probe_sfx", 0x78, 0);
    build_expect(0, "version 120");
    put_bank(BANK, "p6probe_sfx", 0x71, 0);
    put_bank("assets/sound/soundbanks/pc/p6probe_spider.bnk", "p6probe_spider", 0x71, 0);
    build_expect(0, "'_sp'");
    drop("assets/sound/soundbanks/pc/p6probe_spider.bnk");
    put_bank("assets/sound/soundbanks/pc/other_sfx.bnk", "other_sfx", 0x71, 0);
    build_expect(0, "must start with 'p6probe_'");
    drop("assets/sound/soundbanks/pc/other_sfx.bnk");
    put_bank("assets/sound/mods/p6probe/p6probe_x.bnk", "p6probe_x", 0x71, 0);
    build_expect(0, "sound/soundbanks/pc/<stem>.bnk");
    drop("assets/sound/mods/p6probe/p6probe_x.bnk");
    put("mod.json", "{\"id\": \"p6probe\", \"version\": \"1.0.0\", \"name\": \"Sound probe\", "
                    "\"shadows\": [\"sound/soundbanks/pc/p6probe_sfx.bnk\"]}");
    build_expect(0, "cannot be replaced");
    put("mod.json", DESC);
    build_expect(1, "");

    drop(BANK); drop(SND); drop(ENT); drop("mod.json"); drop("bundle.json"); drop("bundle.canonical.json");
    {
        static const char *const dirs[] = {
            "assets\\sound\\soundbanks\\pc", "assets\\sound\\soundbanks", "assets\\sound\\mods\\p6probe",
            "assets\\sound\\mods", "assets\\sound",
            "assets\\generated\\decls\\sound\\mods\\p6probe", "assets\\generated\\decls\\sound\\mods",
            "assets\\generated\\decls\\sound", "assets\\generated\\decls\\entitydef\\mods\\p6probe",
            "assets\\generated\\decls\\entitydef\\mods", "assets\\generated\\decls\\entitydef",
            "assets\\generated\\decls", "assets\\generated", "assets", "", NULL };
        char path[MAX_PATH];
        size_t i;
        for (i = 0; dirs[i]; i++) { _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", g_dir, dirs[i]); RemoveDirectoryA(path); }
        *strrchr(g_dir, '\\') = 0;
        RemoveDirectoryA(g_dir);
    }
}

/* SH_MODSOUND_FIXTURE names a source mod folder (snaphak-re's lane P6f
 * p6probe): build it and print what each file provides and needs. Stock
 * names are taken as installed (the archives are not read here). */
static int any_stock(void *ctx, const char *request) { (void)ctx; return strstr(request, "/mods/") == NULL; }
static void test_fixture(const char *folder)
{
    static const sh_mb_resolver resolver = { NULL, NULL, any_stock, NULL };
    sh_mb_bundle b;
    size_t i, k, banks = 0, sounds = 0;
    int ok = sh_mb_build(folder, &resolver, &b);
    CHECK(ok);
    if (!ok) printf("  fixture: %s\n", b.first_cause);
    for (i = 0; i < b.file_count; i++) {
        const sh_mb_file *f = &b.files[i];
        printf("  %-11s %-44s %8llu", f->ptype, f->pname, f->size);
        for (k = 0; k < f->need_count; k++) printf(" %s%s:%s", k ? "" : "needs ", f->needs[k].type, f->needs[k].name);
        printf("\n");
        if (!strcmp(f->ptype, "soundBank")) banks++;
        if (!strcmp(f->ptype, "sound")) sounds++;
    }
    printf("  fixture %s: %zu files, %zu bank(s), %zu sound decl(s), bundle %s\n", b.id, b.file_count, banks, sounds,
           ok ? b.hash : "(refused)");
    CHECK(banks >= 1 && sounds >= 1);
    sh_mb_free(&b);
}

/* ------------------------------------------------------------ images */

static uint8_t *map_pe_by_rva(const char *path, size_t *image_sz)
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
    *image_sz = nt->OptionalHeader.SizeOfImage;
    free(file);
    return img;
}

typedef struct expect {
    uint32_t loadbanks, submit, open, cmd, sound_system, lb_slot, open_slot_a, open_slot_b;
} expect;
static const expect VK = { 0x19F6340u, 0x19F6A20u, 0x19F6D60u, 0x1689010u, 0x65DAC90u, 0x28DC540u, 0x28DC5A8u, 0x28DC638u };
static const expect GL = { 0x1A123F0u, 0x1A12AD0u, 0x1A12E10u, 0x16895F0u, 0x4EE0250u, 0x290BB20u, 0x290BB88u, 0x290BC18u };

static uint32_t resolve(const uint8_t *base, const char *name, int vulkan)
{
    size_t i;
    sig_result r;
    for (i = 0; BACKEND_ENGINE_SIGNATURES[i].name; i++) {
        const sig_entry *e = &BACKEND_ENGINE_SIGNATURES[i];
        if (strcmp(e->name, name)) continue;
        if (sig_resolve_one(base, e, &r) != SIG_OK) { printf("  %s did not resolve uniquely\n", name); return 0; }
        CHECK(r.rva == (vulkan ? e->rva_vk : e->rva_gl));
        return r.rva;
    }
    printf("  %s is not in the signature table\n", name);
    return 0;
}

static void test_image(const char *path)
{
    size_t size = 0;
    uint8_t *base = map_pe_by_rva(path, &size);
    wchar_t wide[MAX_PATH];
    const sh_build_identity *id;
    const expect *x;
    uint64_t preferred;
    uintptr_t ss = 0, slot = 0;
    void **slots[4];
    int n;
    CHECK(base != NULL);
    if (!base) return;
    MultiByteToWideChar(CP_ACP, 0, path, -1, wide, MAX_PATH);
    id = sh_host_test_bind_image(base, wide);
    CHECK(id != NULL);
    if (!id) { free(base); return; }
    x = id->vulkan ? &VK : &GL;
    CHECK(resolve(base, "AudioLoadBanks", id->vulkan) == x->loadbanks);
    CHECK(resolve(base, "AudioBankSubmit", id->vulkan) == x->submit);
    CHECK(resolve(base, "AudioFileOpenName", id->vulkan) == x->open);
    CHECK(resolve(base, "SoundLoadBanksCmd", id->vulkan) == x->cmd);
    CHECK(sh_modsound_decode_cmd(base + x->cmd, &ss, &slot));
    CHECK(ss - (uintptr_t)base == x->sound_system && slot - ss == 0x140);
    preferred = ((IMAGE_NT_HEADERS64 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew))->OptionalHeader.ImageBase;
    n = sh_modsound_find_slots(base, preferred + x->open, slots, 4);
    CHECK(n == 2 && (uint8_t *)slots[0] - base == x->open_slot_a && (uint8_t *)slots[1] - base == x->open_slot_b);
    n = sh_modsound_find_slots(base, preferred + x->loadbanks, slots, 4);
    CHECK(n == 1 && (uint8_t *)slots[0] - base == x->lb_slot);
    /* The per-bank wrapper is called, never held in a vtable. */
    CHECK(sh_modsound_find_slots(base, preferred + x->submit, slots, 4) == 0);
    printf("%s %s: LoadBanks 0x%x (slot 0x%x), submit 0x%x, open-by-name 0x%x (slots 0x%x, 0x%x), soundSystem 0x%x\n",
           g_fail ? "BAD" : "OK ", id->vulkan ? "Vulkan" : "OpenGL", x->loadbanks, x->lb_slot, x->submit, x->open,
           x->open_slot_a, x->open_slot_b, x->sound_system);
    free(base);
}

int main(int argc, char **argv)
{
    test_names();
    test_decode();
    test_table();
    test_routes();
    test_builder();
    for (int i = 1; i < argc; i++) test_image(argv[i]);
    if (getenv("SH_MODSOUND_FIXTURE")) test_fixture(getenv("SH_MODSOUND_FIXTURE"));
    printf(g_fail ? "modsound_test: FAILED\n" : "modsound_test: all passed\n");
    return g_fail;
}
