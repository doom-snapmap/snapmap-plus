/* modregistry_test.c -- the registries (modregistry.c) and their serving
 * (modserve.c, ms_scope_compute): the text primitives, the extends table and
 * grammar (a list, the md6Def bag, a spider-style subGraph), and generation
 * from a synthetic declared set written to a temporary mods folder with the
 * game's texts given by a fake installed-content reader: the conductor from
 * two packs (bits assigned without collision, the same whatever the install
 * order, encounters and scores served with the assigned bits), the body
 * events of two bundles appended, the player fields of one bundle, a clash
 * with and without the map's choice, a weapon served under the stock name
 * only when declared, and the console verb. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modregistry.h"
#include "modserve.h"
#include "modmanifest.h"
#include "modbundle.h"

static int g_failed, g_checks;
#define CHECK(expr) do {                                                         \
    g_checks++;                                                                  \
    if (!(expr)) {                                                               \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++;                                                              \
    }                                                                            \
} while (0)

static char g_log[262144];
static size_t g_log_len;
void backend_log(const char *message)
{
    size_t n = message ? strlen(message) : 0;
    if (g_log_len + n + 2 < sizeof g_log) {
        memcpy(g_log + g_log_len, message, n);
        g_log_len += n;
        g_log[g_log_len++] = '\n';
        g_log[g_log_len] = 0;
    }
}
static int log_has(const char *needle) { return strstr(g_log, needle) != NULL; }

/* The engine half the manifest calls (modserve_engine.c). */
int modserve_activate_bundles(const char *const *bundle_hashes, int n) { (void)bundle_hashes; (void)n; return 0; }

/* ------------------------------------------------------------------ the game's texts */

#define REQ_CONDUCTOR "generated/decls/entitydef/ai/conductor/coop/snapmap_default.decl"
#define REQ_BODY      "generated/decls/md6def/player/mp/tp_body_mp.md6.decl"
#define REQ_PLAYER    "generated/decls/entitydef/player/coop.decl"
#define REQ_SPIDER    "generated/decls/aifsmmanager/states/spidermastermind.decl"
#define REQ_PISTOL    "generated/decls/weapon/weapon/zion/player/sp/pistol.decl"

static const char STOCK_CONDUCTOR[] =
    "{\r\n\tinherit = \"ai/base/conductor\";\r\n\tedit = {\r\n\t\taiTypeList = {\r\n\t\t\tnum = 2;\r\n"
    "\t\t\titem[0] = {\r\n\t\t\t\taiType = \"SPAWN_AI_TYPE_IMP\";\r\n\t\t\t\tentityDef = \"ai/demon/imp_coop\";\r\n\t\t\t}\r\n"
    "\t\t\titem[1] = {\r\n\t\t\t\taiType = \"SPAWN_AI_TYPE_ARCHVILE\";\r\n\t\t\t\tentityDef = \"ai/demon/archvile_coop\";\r\n\t\t\t}\r\n"
    "\t\t}\r\n\t\tglobalAIMods = {\r\n\t\t\tnum = 1;\r\n\t\t\titem[0] = {\r\n\t\t\t\taiType = \"SPAWN_AI_TYPE_IMP\";\r\n\t\t\t}\r\n"
    "\t\t}\r\n\t}\r\n}";
static const char STOCK_BODY[] =
    "{\n\tinit {\n\t\tmesh \"md6/player/body.md6mesh\"\n\t}\n\tevents {\n\t\tanim \"md6/a.md6anim\" {\n"
    "\t\t\tevent \"ae_x\" {\n\t\t\t\tframe 1\n\t\t\t}\n\t\t}\n\t}\n\taliases {\n\t}\n}\n";
static const char STOCK_PLAYER[] =
    "{\r\n\tinherit = \"player/mp\";\r\n\teditorVars {\r\n\t\tplaceable = false;\r\n\t}\r\n\tedit = {\r\n"
    "\t\tsyncMelee = {\r\n\t\t\tsyncGroups = {\r\n\t\t\t\tnum = 3;\r\n\t\t\t}\r\n\t\t}\r\n\t}\r\n}";
static const char STOCK_SPIDER[] =
    "{\n\tedit = {\n\t\tobject = {\n\t\t\tdebugFlags = 6;\n\t\t}\n\t\tsubGraphs = {\n\t\t\tsubGraph = {\n"
    "\t\t\t\tobject = {\n\t\t\t\t\tname = \"spiderMastermind\";\n\t\t\t\t}\n\t\t\t}\n\t\t}\n\t}\n}\n";
static const char STOCK_PISTOL[] = "{\n\tedit = {\n\t\tdamage = 10;\n\t}\n}\n";

typedef struct stock_entry { const char *request, *text; } stock_entry;
static const stock_entry g_stock[] = {
    { REQ_CONDUCTOR, STOCK_CONDUCTOR }, { REQ_BODY, STOCK_BODY }, { REQ_PLAYER, STOCK_PLAYER },
    { REQ_SPIDER, STOCK_SPIDER }, { REQ_PISTOL, STOCK_PISTOL },
};

static const char *stock_text(const char *source)
{
    size_t i;
    for (i = 0; i < sizeof g_stock / sizeof g_stock[0]; ++i) if (!_stricmp(source, g_stock[i].request)) return g_stock[i].text;
    return NULL;
}
static int st_identify(void *c, const char *source, char sha[SH_MB_HASH_HEX + 1], unsigned long long *size)
{
    const char *t = stock_text(source);
    (void)c;
    if (!t) return SH_MB_STOCK_ABSENT;
    sh_mb_sha256_hex((const unsigned char *)t, strlen(t), sha);
    *size = strlen(t);
    return SH_MB_STOCK_OK;
}
static int st_read(void *c, const char *source, const char *sha, unsigned char **bytes, size_t *len)
{
    const char *t = stock_text(source);
    char h[SH_MB_HASH_HEX + 1];
    (void)c;
    if (!t) return SH_MB_STOCK_ABSENT;
    sh_mb_sha256_hex((const unsigned char *)t, strlen(t), h);
    if (sha && strcmp(sha, h)) return SH_MB_STOCK_MISMATCH;
    *len = strlen(t);
    *bytes = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, *len + 1);
    memcpy(*bytes, t, *len);
    return SH_MB_STOCK_OK;
}
static int st_verify(void *c, const char *source, const char *sha, unsigned long long *size)
{
    char h[SH_MB_HASH_HEX + 1];
    unsigned long long n;
    if (st_identify(c, source, h, &n) != SH_MB_STOCK_OK) return SH_MB_STOCK_ABSENT;
    if (strcmp(h, sha)) return SH_MB_STOCK_MISMATCH;
    *size = n;
    return SH_MB_STOCK_OK;
}

/* ------------------------------------------------------------------ text primitives */

static int count_of(const char *hay, const char *needle)
{
    int n = 0;
    while ((hay = strstr(hay, needle)) != NULL) { n++; hay++; }
    return n;
}

static void row_cb(void *c, const char *ai, const char *def)
{
    char *out = (char *)c;
    strcat_s(out, 1024, ai);
    strcat_s(out, 1024, "=");
    strcat_s(out, 1024, def);
    strcat_s(out, 1024, ";");
}

static void test_text(void)
{
    const char *defs[2] = { "mods/a.boss/ai/staff", "mods/a.boss/ai/hammer" };
    unsigned bits[2] = { 524288u, 1048576u };
    size_t len = 0;
    char *t, rows[1024] = "";
    const char *values[SH_MB_PLAYER_FIELDS];
    unsigned from[2] = { 2097152u, 4194304u }, to[2] = { 524288u, 4194304u };
    printf("-- registries: conductor, player and bit texts\n");
    /* The conductor: two items appended, num raised, the rest byte for byte. */
    t = sh_modregistry_conductor_text(STOCK_CONDUCTOR, strlen(STOCK_CONDUCTOR), defs, bits, 2, &len);
    if (getenv("MR_DUMP") && t) fwrite(t, 1, len, stdout);
    CHECK(t && strstr(t, "\t\t\tnum = 4;\r\n"));
    CHECK(t && strstr(t, "\t\t\titem[2] = {\r\n\t\t\t\taiType = \"524288\";\r\n\t\t\t\tentityDef = \"mods/a.boss/ai/staff\";\r\n\t\t\t}\r\n"
                        "\t\t\titem[3] = {\r\n\t\t\t\taiType = \"1048576\";\r\n\t\t\t\tentityDef = \"mods/a.boss/ai/hammer\";\r\n\t\t\t}\r\n\t\t}\r\n\t\tglobalAIMods"));
    CHECK(t && !strncmp(t, STOCK_CONDUCTOR, strstr(STOCK_CONDUCTOR, "num = 2") - STOCK_CONDUCTOR + 6));
    CHECK(t && !strcmp(t + len - strlen(strstr(STOCK_CONDUCTOR, "\t\t}\r\n\t\tglobalAIMods")), strstr(STOCK_CONDUCTOR, "\t\t}\r\n\t\tglobalAIMods")));
    CHECK(t && sh_modregistry_conductor_rows(t, len, row_cb, rows) == 4 &&
          !strcmp(rows, "SPAWN_AI_TYPE_IMP=ai/demon/imp_coop;SPAWN_AI_TYPE_ARCHVILE=ai/demon/archvile_coop;"
                        "524288=mods/a.boss/ai/staff;1048576=mods/a.boss/ai/hammer;"));
    free(t);
    CHECK(sh_modregistry_conductor_text("{ edit = { } }", 14, defs, bits, 1, &len) == NULL);
    /* Bits: memberType and aiType values only, every number in a flag list. */
    {
        static const char RW[] = "{ edit = { memberList = { item[0] = { memberType = \"2097152 SPAWN_AI_TYPE_IMP 4194304\"; "
                                 "numToSpawn = 2097152; } } aiType = \"2097152\"; other = \"2097152\"; } }";
        t = sh_modregistry_rewrite_bits(RW, sizeof RW - 1, from, to, 2, &len);
    }
    if (0) t = sh_modregistry_rewrite_bits("{ edit = { memberList = { item[0] = { memberType = \"2097152 SPAWN_AI_TYPE_IMP 4194304\"; "
                                    "numToSpawn = 2097152; } } aiType = \"2097152\"; other = \"2097152\"; } }", 0, from, to, 2, &len);
    CHECK(t && strstr(t, "memberType = \"524288 SPAWN_AI_TYPE_IMP 4194304\"") && strstr(t, "aiType = \"524288\"") &&
          strstr(t, "numToSpawn = 2097152;") && strstr(t, "other = \"2097152\""));
    free(t);
    CHECK(sh_modregistry_rewrite_bits("{ aiType = \"8\"; }", 17, from, to, 2, &len) == NULL);
    CHECK(sh_modregistry_stock_bit(262144u) && sh_modregistry_stock_bit(1u << 30) && !sh_modregistry_stock_bit(524288u) &&
          !sh_modregistry_stock_bit(3u));
    /* The player entity: fields added first in edit, an existing one replaced. */
    memset((void *)values, 0, sizeof values);
    values[SH_MB_PLAYER_HANDS] = "player/fp_hands.md6";
    values[SH_MB_PLAYER_BODY] = "mods/bare-hands/player_tp_body_bare";
    values[SH_MB_PLAYER_HANDS_ANIM_WEB] = "mods/w.hands/player/fp_hands_mp";
    t = sh_modregistry_player_text(STOCK_PLAYER, strlen(STOCK_PLAYER), values, &len);
    CHECK(t && strstr(t, "\tedit = {\r\n\t\thandsModelMPUseDoomHands = \"player/fp_hands.md6\";\r\n"
                         "\t\tthirdPersonBodyMPUseDoomHands = \"mods/bare-hands/player_tp_body_bare\";\r\n"
                         "\t\thandsAnimWeb = \"mods/w.hands/player/fp_hands_mp\";\r\n\t\tsyncMelee = {"));
    CHECK(t && strstr(t, "\t\t\t\tnum = 3;\r\n"));   /* the rest untouched (a list the composer would renumber) */
    if (t) {
        char *again;
        values[SH_MB_PLAYER_HANDS] = "mods/other/hands.md6";
        values[SH_MB_PLAYER_BODY] = values[SH_MB_PLAYER_HANDS_ANIM_WEB] = NULL;
        again = sh_modregistry_player_text(t, len, values, &len);
        CHECK(again && strstr(again, "handsModelMPUseDoomHands = \"mods/other/hands.md6\";") &&
              count_of(again, "handsModelMPUseDoomHands =") == 1);
        free(again);
    }
    free(t);
    CHECK(!strcmp(sh_modregistry_player_key(SH_MB_PLAYER_HANDS_D1), "handsModelMPUseDoomHands_D1"));
    /* Slots. */
    CHECK(sh_modregistry_slot_valid("player.hands") && sh_modregistry_slot_valid("player.handsAnimWeb") &&
          sh_modregistry_slot_valid("weapons/weapon/zion/player/sp/pistol") && !sh_modregistry_slot_valid("player.bodyEvents") &&
          !sh_modregistry_slot_valid("weapons/") && !sh_modregistry_slot_valid("player.loadout"));
}

static void test_extends(void)
{
    int kind = 0, fixed = 9;
    const char *cont = NULL;
    char cause[256], *t, *n;
    size_t len = 0, nlen = 0;
    int added = 0;
    static const char PART1[] =
        "{\n\tevents {\n\t\tanim \"md6/mods/k1/scene.md6anim\" {\n\t\t\tevent \"ae_hide\" {\n\t\t\t\tframe 1\n\t\t\t}\n\t\t}\n\t}\n"
        "\taliases {\n\t\talias { name \"mods/k1/x\" anim \"md6/mods/k1/x.md6anim\" }\n\t}\n}\n";
    static const char PART2[] =   /* the same scene clip (a second pack of one family) and one more */
        "{\n\tevents {\n\t\tanim \"md6/mods/k1/scene.md6anim\" {\n\t\t\tevent \"ae_hide\" {\n\t\t\t\tframe 1\n\t\t\t}\n\t\t}\n"
        "\t\tanim \"md6/mods/k2/other.md6anim\" {\n\t\t\tevent \"ae_show\" {\n\t\t\t\tframe 9\n\t\t\t}\n\t\t}\n\t}\n}\n";
    static const char CLASH[] =
        "{\n\tevents {\n\t\tanim \"md6/a.md6anim\" {\n\t\t\tevent \"ae_x\" {\n\t\t\t\tframe 2\n\t\t\t}\n\t\t}\n\t}\n}\n";
    static const char OUTSIDE[] = "{\n\tinit {\n\t\tmesh \"x\"\n\t}\n}\n";
    static const char SPIDER_PART[] =
        "{\n\tedit = {\n\t\tsubGraphs = {\n\t\t\tsubGraph = {\n\t\t\t\tobject = {\n\t\t\t\t\tname = \"smm_root\";\n"
        "\t\t\t\t\tisRoot = false;\n\t\t\t\t}\n\t\t\t}\n\t\t}\n\t}\n}\n";
    static const char SPIDER_EXPECT[] =
        "{\n\tedit = {\n\t\tobject = {\n\t\t\tdebugFlags = 6;\n\t\t}\n\t\tsubGraphs = {\n\t\t\tsubGraph = {\n"
        "\t\t\t\tobject = {\n\t\t\t\t\tname = \"spiderMastermind\";\n\t\t\t\t}\n\t\t\t}\n"
        "\t\t\tsubGraph = {\n\t\t\t\tobject = {\n\t\t\t\t\tname = \"smm_root\";\n\t\t\t\t\tisRoot = false;\n\t\t\t\t}\n\t\t\t}\n"
        "\t\t}\n\t}\n}\n";
    static const char WL[] = "{\n\tedit = {\n\t\tvalidEncounters = {\n\t\t\tnum = 2;\n\t\t\titem[0] = \"e2_a\";\n\t\t\titem[1] = \"e2_b\";\n\t\t}\n\t}\n}\n";
    static const char WL_PART[] = "{ edit = { validEncounters = { num = 2; item[0] = \"e2_b\"; item[1] = \"mods/k1/single_boss\"; } } }";
    printf("-- registries: extends, the table and the list and bag grammars\n");
    CHECK(sh_modregistry_extend_target("entitydef/ai/conductor/coop/snapmap_default", &kind, &cont, &fixed, cause, sizeof cause) &&
          kind == SH_MR_LIST && !strcmp(cont, "edit/aiTypeList") && fixed == SH_MR_CONDUCTOR);
    CHECK(sh_modregistry_extend_target("AIFSMManager/states/spidermastermind", &kind, &cont, &fixed, cause, sizeof cause) &&
          kind == SH_MR_BAG && !strcmp(cont, "edit/subGraphs") && fixed == -1);
    CHECK(sh_modregistry_extend_target("md6def/player/mp/tp_body_mp.md6", &kind, &cont, &fixed, NULL, 0) && fixed == SH_MR_BODY &&
          !strcmp(cont, "events;aliases"));
    CHECK(sh_modregistry_extend_target("snappropertyinspector_whitelistencounterdecllist/boss_encounters_white_list_list_inspector",
                                       &kind, NULL, NULL, NULL, 0) && kind == SH_MR_LIST);
    CHECK(!sh_modregistry_extend_target("aifsmmanager/states/a/b", NULL, NULL, NULL, cause, sizeof cause));
    CHECK(!sh_modregistry_extend_target("entitydef/player/coop", NULL, NULL, NULL, cause, sizeof cause) &&
          !strcmp(cause, "entitydef/player/coop is not a list the product extends"));
    CHECK(sh_modregistry_extend_request("AIFSMManager/states/spidermastermind", cause, sizeof cause) && !strcmp(cause, REQ_SPIDER));

    /* The md6Def bag: entries appended, the identical one skipped, the empty aliases filled. */
    t = sh_modserve_compose_extend(STOCK_BODY, strlen(STOCK_BODY), PART1, strlen(PART1), "events;aliases", SH_MR_BAG, &len, &added);
    CHECK(t && added == 2 && strstr(t, "anim \"md6/mods/k1/scene.md6anim\" {") && strstr(t, "alias {") &&
          strstr(t, "mesh \"md6/player/body.md6mesh\""));
    n = t ? sh_modserve_compose_extend(t, len, PART2, strlen(PART2), "events;aliases", SH_MR_BAG, &nlen, &added) : NULL;
    CHECK(n && added == 1 && count_of(n, "anim \"md6/mods/k1/scene.md6anim\"") == 1 && strstr(n, "md6/mods/k2/other.md6anim"));
    free(n);
    free(t);
    /* A named entry the game's text holds with other content, and members outside the containers, are refused. */
    CHECK(sh_modserve_compose_extend(STOCK_BODY, strlen(STOCK_BODY), CLASH, strlen(CLASH), "events;aliases", SH_MR_BAG, &len, &added) == NULL);
    CHECK(sh_modserve_compose_extend(STOCK_BODY, strlen(STOCK_BODY), OUTSIDE, strlen(OUTSIDE), "events;aliases", SH_MR_BAG, &len, &added) == NULL);

    /* A spider-style subGraph: the game's text plus the appended block. */
    t = sh_modserve_compose_extend(STOCK_SPIDER, strlen(STOCK_SPIDER), SPIDER_PART, strlen(SPIDER_PART), "edit/subGraphs",
                                   SH_MR_BAG, &len, &added);
    {
        const char *one[1] = { SPIDER_EXPECT };
        size_t el = strlen(SPIDER_EXPECT), nl2 = 0;
        int a2 = 0;
        char *norm = sh_modserve_compose_texts(one, &el, 1, &nl2, &a2);   /* the composer's own spelling of the expectation */
        CHECK(t && added == 1 && norm && !strcmp(t, norm));
        CHECK(norm && !strcmp(norm, SPIDER_EXPECT));
        free(norm);
    }
    free(t);
    /* The same partial twice adds it once. */
    t = sh_modserve_compose_extend(SPIDER_EXPECT, strlen(SPIDER_EXPECT), SPIDER_PART, strlen(SPIDER_PART), "edit/subGraphs",
                                   SH_MR_BAG, &len, &added);
    CHECK(t && added == 0 && count_of(t, "subGraph = {") == 2);
    free(t);
    /* A list: the new item appended and renumbered, the one the game lists skipped. */
    t = sh_modserve_compose_extend(WL, strlen(WL), WL_PART, strlen(WL_PART), "edit/validEncounters", SH_MR_LIST, &len, &added);
    CHECK(t && added == 1 && strstr(t, "num = 3;") && strstr(t, "item[2] = \"mods/k1/single_boss\";"));
    free(t);
}

/* ------------------------------------------------------------------ a synthetic declared set */

static char g_top[MAX_PATH];

static void mkdirs(const char *path)
{
    char tmp[MAX_PATH], *p;
    strcpy_s(tmp, sizeof tmp, path);
    for (p = tmp + 3; *p; ++p)
        if (*p == '\\') { *p = 0; CreateDirectoryA(tmp, NULL); *p = '\\'; }
    CreateDirectoryA(tmp, NULL);
}

static void write_file(const char *path, const char *body)
{
    char dir[MAX_PATH], *slash;
    FILE *f;
    strcpy_s(dir, sizeof dir, path);
    slash = strrchr(dir, '\\');
    if (slash) { *slash = 0; mkdirs(dir); }
    if (fopen_s(&f, path, "wb") || !f) { g_failed++; return; }
    fwrite(body, 1, strlen(body), f);
    fclose(f);
}

typedef struct fx { const char *request, *ptype, *pname, *body; } fx;

/* <root>\<folder>\mod.json with an author block (`author`: members with a
 * trailing comma), a product block (`registries` JSON or NULL, `shadows`)
 * and the files. The bundle hash is 64 x `digit`. */
static void make_mod(const char *root, const char *folder, const char *id, char digit, const char *author,
                     const char *registries, const char *shadows, const fx *files, int n, int legacy)
{
    static char json[65536];
    char dir[MAX_PATH], path[MAX_PATH], h[65], *p;
    size_t at = 0;
    int i;
    memset(h, digit, 64);
    h[64] = 0;
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\%s", root, folder);
    mkdirs(dir);
    at += _snprintf_s(json + at, sizeof json - at, _TRUNCATE,
                      "{\n  \"id\": \"%s\",\n  \"version\": \"1.0.0\",\n  \"name\": \"%s\",\n  %s%s\n"
                      "  \"snapmap-plus\": {\"format\": 1, \"build\": 1, \"bundle\": \"%s\", \"built\": %d%s%s%s,\n"
                      "    \"shadows\": [%s],\n    \"files\": [\n",
                      id, id, author ? author : "", legacy ? "\"legacy\": {}," : "", h, 100 + digit,
                      registries ? ", \"registries\": " : "", registries ? registries : "", "", shadows ? shadows : "");
    for (i = 0; i < n; ++i) {
        char sha[65];
        sh_mb_sha256_hex((const unsigned char *)files[i].body, strlen(files[i].body), sha);
        at += _snprintf_s(json + at, sizeof json - at, _TRUNCATE,
                          "      {\"path\": \"assets/%s\", \"sha256\": \"%s\", \"size\": %zu, "
                          "\"provides\": {\"type\": \"%s\", \"name\": \"%s\"}, \"needs\": []}%s\n",
                          files[i].request, sha, strlen(files[i].body), files[i].ptype, files[i].pname, i + 1 < n ? "," : "");
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\assets\\%s", dir, files[i].request);
        for (p = path; *p; ++p) if (*p == '/') *p = '\\';
        write_file(path, files[i].body);
    }
    at += _snprintf_s(json + at, sizeof json - at, _TRUNCATE, "    ]\n  }\n}\n");
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\mod.json", dir);
    write_file(path, json);
}

static int open_text(const char *name, char *out, size_t cap)
{
    unsigned char *bytes = NULL;
    size_t len = 0;
    int r = sh_modserve_open(name, &bytes, &len);
    out[0] = 0;
    if (r > 0) {
        size_t n = len < cap - 1 ? len : cap - 1;
        memcpy(out, bytes, n);
        out[n] = 0;
        HeapFree(GetProcessHeap(), 0, bytes);
    }
    return r;
}

static void report_line(void *c, const char *line)
{
    strcat_s((char *)c, 16384, line);
    strcat_s((char *)c, 16384, "\n");
}

static const char *hash_of(char digit, char *out)
{
    memset(out, digit, 64);
    out[64] = 0;
    return out;
}

/* The boss packs (legacy, each carrying a conductor copy with bits the authors
 * chose, one of them the same bit) and two keyed bundles. */
#define BOSS_CONDUCTOR(n, items) \
    "{\r\n\tinherit = \"ai/base/conductor\";\r\n\tedit = {\r\n\t\taiTypeList = {\r\n\t\t\tnum = " n ";\r\n" \
    "\t\t\titem[0] = {\r\n\t\t\t\taiType = \"SPAWN_AI_TYPE_IMP\";\r\n\t\t\t\tentityDef = \"ai/demon/imp_coop\";\r\n\t\t\t}\r\n" \
    "\t\t\titem[1] = {\r\n\t\t\t\taiType = \"SPAWN_AI_TYPE_ARCHVILE\";\r\n\t\t\t\tentityDef = \"ai/demon/archvile_coop\";\r\n\t\t\t}\r\n" \
    items "\t\t}\r\n\t\tglobalAIMods = {\r\n\t\t\tnum = 1;\r\n\t\t\titem[0] = {\r\n\t\t\t\taiType = \"SPAWN_AI_TYPE_IMP\";\r\n\t\t\t}\r\n" \
    "\t\t}\r\n\t}\r\n}"
#define CITEM(n, bit, def) "\t\t\titem[" #n "] = {\r\n\t\t\t\taiType = \"" bit "\";\r\n\t\t\t\tentityDef = \"" def "\";\r\n\t\t\t}\r\n"

#define GUARD_SHADOWS "\"" REQ_CONDUCTOR "\", \"generated/decls/snapaiencounter/single_hell_guard_staff.decl\", "     "\"generated/decls/snapaiencounter/single_hell_guard_hammer.decl\", \"generated/decls/snapaiscore/hell_guard_staff.decl\""
#define SPIDER_SHADOWS "\"" REQ_CONDUCTOR "\", \"generated/decls/snapaiencounter/single_spider_mastermind.decl\""

static void build_set(const char *root, int reverse)
{
    static const fx guard[] = {
        { REQ_CONDUCTOR, "entityDef", "ai/conductor/coop/snapmap_default",
          BOSS_CONDUCTOR("4", CITEM(2, "2097152", "ai/demon/talismanguard/staff") CITEM(3, "4194304", "ai/demon/talismanguard/hammer")) },
        { "generated/decls/snapaiencounter/single_hell_guard_staff.decl", "snapAiEncounter", "single_hell_guard_staff",
          "{\n\tedit = {\n\t\tmemberList = {\n\t\t\tnum = 1;\n\t\t\titem[0] = {\n\t\t\t\tmemberType = \"2097152\";\n\t\t\t}\n\t\t}\n\t}\n}" },
        { "generated/decls/snapaiencounter/single_hell_guard_hammer.decl", "snapAiEncounter", "single_hell_guard_hammer",
          "{\n\tedit = {\n\t\tmemberList = {\n\t\t\tnum = 1;\n\t\t\titem[0] = {\n\t\t\t\tmemberType = \"4194304\";\n\t\t\t}\n\t\t}\n\t}\n}" },
        { "generated/decls/snapaiscore/hell_guard_staff.decl", "snapAiScore", "hell_guard_staff",
          "{\n\tedit = {\n\t\taiType = \"2097152\";\n\t\tbaseScore = 1200;\n\t}\n}" },
    };
    static const fx spider[] = {
        { REQ_CONDUCTOR, "entityDef", "ai/conductor/coop/snapmap_default",
          BOSS_CONDUCTOR("3", CITEM(2, "2097152", "ai/demon/spider_mastermind_base")) },
        { "generated/decls/snapaiencounter/single_spider_mastermind.decl", "snapAiEncounter", "single_spider_mastermind",
          "{\n\tedit = {\n\t\tmemberList = {\n\t\t\tnum = 1;\n\t\t\titem[0] = {\n\t\t\t\tmemberType = \"2097152\";\n\t\t\t}\n\t\t}\n\t}\n}" },
    };
    static const fx scenes[] = {   /* bodyEvents and an extends of the spider states */
        { "generated/decls/md6def/mods/k.scenes/body_events.md6.decl", "md6Def", "mods/k.scenes/body_events.md6",
          "{\n\tevents {\n\t\tanim \"md6/mods/k.scenes/a.md6anim\" {\n\t\t\tevent \"ae_hide\" {\n\t\t\t\tframe 1\n\t\t\t}\n\t\t}\n\t}\n"
          "\taliases {\n\t\talias { name \"mods/k.scenes/x\" anim \"md6/mods/k.scenes/x.md6anim\" }\n\t}\n}\n" },
        { "generated/decls/aifsmmanager/mods/k.scenes/smm_root.decl", "aiFSMManager", "mods/k.scenes/smm_root",
          "{\n\tedit = {\n\t\tsubGraphs = {\n\t\t\tsubGraph = {\n\t\t\t\tobject = {\n\t\t\t\t\tname = \"smm_root\";\n\t\t\t\t}\n\t\t\t}\n\t\t}\n\t}\n}\n" },
    };
    static const fx hands[] = {   /* player fields, bodyEvents, a weapon */
        { "generated/decls/md6def/mods/k.hands/body_events.md6.decl", "md6Def", "mods/k.hands/body_events.md6",
          "{\n\tevents {\n\t\tanim \"md6/mods/k.hands/b.md6anim\" {\n\t\t\tevent \"ae_show\" {\n\t\t\t\tframe 4\n\t\t\t}\n\t\t}\n\t}\n}\n" },
        { "generated/decls/entitydef/mods/k.hands/body.decl", "entityDef", "mods/k.hands/body", "{ inherit = \"player_tp_body_mp\"; }" },
        { "generated/decls/weapon/mods/k.hands/weapon/pistol.decl", "weapon", "mods/k.hands/weapon/pistol",
          "{\n\tedit = {\n\t\tdamage = 99;\n\t}\n}\n" },
    };
    static const fx rival[] = {   /* sets player.hands too */
        { "generated/decls/entitydef/mods/k.rival/thing.decl", "entityDef", "mods/k.rival/thing", "{ }" },
    };
    int order[5] = { 0, 1, 2, 3, 4 }, i;
    mkdirs(root);
    for (i = 0; i < 5; ++i) {
        int k = reverse ? order[4 - i] : order[i];
        if (k == 0) make_mod(root, reverse ? "z-guard" : "a-guard", "local.hell-guard", '1', NULL, NULL, GUARD_SHADOWS, guard, 4, 1);
        if (k == 1) make_mod(root, reverse ? "y-spider" : "b-spider", "local.spider-mastermind", '2', NULL, NULL, SPIDER_SHADOWS, spider, 2, 1);
        if (k == 2)
            make_mod(root, reverse ? "x-scenes" : "c-scenes", "k.scenes", '3',
                     "\"player\": {\"bodyEvents\": \"mods/k.scenes/body_events.md6\"},",
                     "{\"extends\": {\"aifsmmanager/states/spidermastermind\": \"mods/k.scenes/smm_root\"}}", NULL, scenes, 2, 0);
        if (k == 3)
            make_mod(root, reverse ? "w-hands" : "d-hands", "k.hands", '4',
                     "\"player\": {\"hands\": \"player/fp_hands.md6\", \"body\": \"mods/k.hands/body\", "
                     "\"bodyEvents\": \"mods/k.hands/body_events.md6\", "
                     "\"weapons\": {\"weapon/zion/player/sp/pistol\": \"mods/k.hands/weapon/pistol\"}},", NULL, NULL, hands, 3, 0);
        if (k == 4)
            make_mod(root, reverse ? "v-rival" : "e-rival", "k.rival", '5', "\"player\": {\"hands\": \"mods/k.rival/hands.md6\"},",
                     NULL, NULL, rival, 1, 0);
    }
}

static void conductor_lines(const char *text, char *out, size_t cap)
{
    out[0] = 0;
    (void)sh_modregistry_conductor_rows(text, strlen(text), row_cb, out);
    (void)cap;
}

static void test_generation(void)
{
    static char text[65536], rep[16384], rows1[1024], rows2[1024];
    char root[MAX_PATH], root2[MAX_PATH], hg[65], hs[65], hsc[65], hh[65], hr[65], key[80], line[512];
    const char *list[5];
    sh_mb_stock_ops ops;
    sh_modserve_hit hit;
    printf("-- registries: generated per map from the declared set\n");
    memset(&ops, 0, sizeof ops);
    ops.identify = st_identify;
    ops.read = st_read;
    ops.verify = st_verify;
    sh_mb_set_stock_ops(&ops);
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\set-a\\mods", g_top);
    _snprintf_s(root2, sizeof root2, _TRUNCATE, "%s\\set-b\\mods", g_top);
    build_set(root, 0);
    build_set(root2, 1);
    hash_of('1', hg); hash_of('2', hs); hash_of('3', hsc); hash_of('4', hh); hash_of('5', hr);

    sh_modserve_scope_enable(1);
    CHECK(sh_modserve_index_build(root) == 5);
    /* No map open: the editor-time boss packs (their shadows are grandfathered
     * conductor copies, not editor decls, so they are not editor-time) and
     * nothing declared: the game's files. */
    CHECK(sh_modserve_scope_set(NULL, 0, NULL, 0, 0) >= 0);
    CHECK(open_text(REQ_PLAYER, text, sizeof text) <= 0);
    CHECK(!sh_modserve_lookup(REQ_PISTOL, &hit));

    /* A map declaring both boss packs: bits assigned from 1 << 19, no collision,
     * in row order then by encounter name; the encounters and the score served
     * with them. */
    list[0] = hg; list[1] = hs;
    CHECK(sh_modserve_scope_set(list, 2, NULL, 0, 1) >= 2);
    CHECK(open_text(REQ_CONDUCTOR, text, sizeof text) == 1);
    conductor_lines(text, rows1, sizeof rows1);
    CHECK(!strcmp(rows1, "SPAWN_AI_TYPE_IMP=ai/demon/imp_coop;SPAWN_AI_TYPE_ARCHVILE=ai/demon/archvile_coop;"
                         "524288=ai/demon/talismanguard/hammer;1048576=ai/demon/talismanguard/staff;"
                         "2097152=ai/demon/spider_mastermind_base;") || (printf("%s\n", rows1), 0));
    CHECK(sh_modserve_lookup(REQ_CONDUCTOR, &hit) && !strcmp(hit.bundle_id, "snapmap-plus.registries"));
    CHECK(open_text("generated/decls/snapaiencounter/single_hell_guard_staff.decl", text, sizeof text) == 1 &&
          strstr(text, "memberType = \"1048576\""));
    CHECK(open_text("generated/decls/snapaiencounter/single_hell_guard_hammer.decl", text, sizeof text) == 1 &&
          strstr(text, "memberType = \"524288\""));
    CHECK(open_text("generated/decls/snapaiscore/hell_guard_staff.decl", text, sizeof text) == 1 && strstr(text, "aiType = \"1048576\""));
    CHECK(open_text("generated/decls/snapaiencounter/single_spider_mastermind.decl", text, sizeof text) == 1 &&
          strstr(text, "memberType = \"2097152\""));   /* its authored bit was the hell guard's: now its own */
    /* The other row order swaps them, deterministically. */
    list[0] = hs; list[1] = hg;
    CHECK(sh_modserve_scope_set(list, 2, NULL, 0, 1) >= 1);
    CHECK(open_text(REQ_CONDUCTOR, text, sizeof text) == 1);
    conductor_lines(text, rows2, sizeof rows2);
    CHECK(strstr(rows2, "524288=ai/demon/spider_mastermind_base;") != NULL);
    CHECK(open_text("generated/decls/snapaiencounter/single_spider_mastermind.decl", text, sizeof text) == 1 &&
          strstr(text, "memberType = \"524288\""));

    /* The same manifest on an install in the opposite folder order: the same texts. */
    list[0] = hg; list[1] = hs;
    CHECK(sh_modserve_index_build(root2) == 5);
    CHECK(sh_modserve_scope_set(list, 2, NULL, 0, 1) >= 0);
    CHECK(open_text(REQ_CONDUCTOR, text, sizeof text) == 1);
    conductor_lines(text, rows2, sizeof rows2);
    CHECK(!strcmp(rows1, rows2));
    CHECK(sh_modserve_index_build(root) == 5);

    /* Body events from two bundles, the player fields of one, its weapon under
     * the stock name, the spider states extended. */
    list[0] = hsc; list[1] = hh;
    CHECK(sh_modserve_scope_set(list, 2, NULL, 0, 1) >= 2);
    CHECK(open_text(REQ_BODY, text, sizeof text) == 1 && strstr(text, "md6/mods/k.scenes/a.md6anim") &&
          strstr(text, "md6/mods/k.hands/b.md6anim") && strstr(text, "anim \"md6/a.md6anim\"") && strstr(text, "mods/k.scenes/x"));
    CHECK(open_text(REQ_PLAYER, text, sizeof text) == 1 && strstr(text, "handsModelMPUseDoomHands = \"player/fp_hands.md6\";") &&
          strstr(text, "thirdPersonBodyMPUseDoomHands = \"mods/k.hands/body\";") && strstr(text, "inherit = \"player/mp\";"));
    CHECK(open_text(REQ_PISTOL, text, sizeof text) == 1 && strstr(text, "damage = 99;"));
    CHECK(sh_modserve_lookup(REQ_PISTOL, &hit) && !strcmp(hit.bundle_id, "snapmap-plus.registries"));
    CHECK(open_text(REQ_SPIDER, text, sizeof text) == 1 && strstr(text, "spiderMastermind") && strstr(text, "smm_root"));
    rep[0] = 0;
    CHECK(sh_modserve_registry_report(report_line, rep) > 0);
    CHECK(strstr(rep, "registry " REQ_PLAYER ": generated") && strstr(rep, "player body: 2 entries from k.scenes@1.0.0") &&
          strstr(rep, "player body: 1 entry from k.hands@1.0.0"));
    /* Played: the same (the declared set serves in Play). */
    CHECK(sh_modserve_scope_mode(SH_MODSERVE_MODE_PLAY) >= 0);
    CHECK(open_text(REQ_PISTOL, text, sizeof text) == 1 && strstr(text, "damage = 99;"));
    (void)sh_modserve_scope_mode(SH_MODSERVE_MODE_EDITOR);

    /* The weapon serves only when its bundle is declared. */
    list[0] = hsc;
    CHECK(sh_modserve_scope_set(list, 1, NULL, 0, 1) >= 1);
    CHECK(!sh_modserve_lookup(REQ_PISTOL, &hit) && open_text(REQ_PISTOL, text, sizeof text) <= 0);
    CHECK(open_text(REQ_PLAYER, text, sizeof text) <= 0);   /* no player intent declared: the game's */

    /* A clash: row order without a choice, one line naming it; the map's choice wins. */
    list[0] = hh; list[1] = hr;
    g_log_len = 0; g_log[0] = 0;
    CHECK(sh_modserve_scope_set(list, 2, NULL, 0, 1) >= 2);
    CHECK(open_text(REQ_PLAYER, text, sizeof text) == 1 && strstr(text, "handsModelMPUseDoomHands = \"player/fp_hands.md6\";"));
    CHECK(log_has("player.hands: set by k.hands@1.0.0 (4444444444444444), k.rival@1.0.0 (5555555555555555); serving "
                  "k.hands@1.0.0 (4444444444444444) (row order; choose with sh_mods choose \"player.hands\" \"<key>\")"));
    CHECK(sh_modregistry_choose_command("player.hands", "k.rival", line, sizeof line) >= 1 &&
          strstr(line, "player.hands is served from k.rival"));
    CHECK(open_text(REQ_PLAYER, text, sizeof text) == 1 && strstr(text, "handsModelMPUseDoomHands = \"mods/k.rival/hands.md6\";") &&
          strstr(text, "thirdPersonBodyMPUseDoomHands = \"mods/k.hands/body\";"));
    CHECK(sh_modregistry_choices_get("player.hands", key, sizeof key) && !strcmp(key, "k.rival"));
    CHECK(sh_modregistry_choose_command("player.loadout", "k.rival", line, sizeof line) == -1 && strstr(line, "not a registry slot"));
    CHECK(sh_modregistry_choose_command("player.hands", "", line, sizeof line) >= 1);
    CHECK(open_text(REQ_PLAYER, text, sizeof text) == 1 && strstr(text, "player/fp_hands.md6"));

    CHECK(sh_modserve_scope_set(NULL, 0, NULL, 0, 0) >= 0);
    sh_modmanifest_reset();
    sh_modserve_scope_enable(0);
    sh_mb_set_stock_ops(NULL);
}

int main(void)
{
    char tmp[MAX_PATH];
    printf("modregistry_test\n");
    GetTempPathA(sizeof tmp, tmp);
    _snprintf_s(g_top, sizeof g_top, _TRUNCATE, "%ssmp-modregistry-%lu", tmp, GetCurrentProcessId());
    mkdirs(g_top);
    test_text();
    test_extends();
    test_generation();
    printf("%s -- %d checks, %d failed\n", g_failed ? "FAILED" : "ok", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
