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

/* ------------------------------------------------------------------ body events merged per entry */

typedef struct merge_tally { int clips, rows; } merge_tally;
static void merge_cb(void *c, const char *key, size_t key_n, int rows, int kept)
{
    merge_tally *t = (merge_tally *)c;
    (void)key; (void)key_n; (void)kept;
    t->clips++;
    t->rows += rows;
}

static char *read_all(const char *path, size_t *len)
{
    FILE *f = NULL;
    char *b;
    long n;
    if (fopen_s(&f, path, "rb") || !f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = (char *)malloc((size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    if (b) { b[n] = 0; *len = (size_t)n; }
    return b;
}

/* The distinct `anim "<clip>"` names of the texts: the clip blocks sit
 * exactly two tabs deep in both the partials and the composer's spelling. */
static int distinct_clips(const char *const *texts, int n)
{
    static char seen[512][300];
    int count = 0, i;
    for (i = 0; i < n; ++i) {
        const char *p = texts[i];
        while ((p = strstr(p, "\n\t\tanim \"")) != NULL) {
            const char *s = p + 9, *e = strchr(s, '"');
            int k, dup = 0;
            p = s;
            if (!e || e - s >= 300) continue;
            for (k = 0; k < count && !dup; ++k) dup = strlen(seen[k]) == (size_t)(e - s) && !strncmp(seen[k], s, (size_t)(e - s));
            if (dup || count >= 512) continue;
            memcpy(seen[count], s, (size_t)(e - s));
            seen[count][e - s] = 0;
            count++;
        }
    }
    return count;
}

static void test_body_rows(const char *mods)
{
    static const char CLASH[] =   /* the game's clip with one more row and one it already holds */
        "{\n\tevents {\n\t\tanim \"md6/a.md6anim\" {\n\t\t\tevent \"ae_x\" {\n\t\t\t\tframe 1\n\t\t\t}\n"
        "\t\t\tevent \"ae_x\" {\n\t\t\t\tframe 2\n\t\t\t}\n\t\t}\n\t}\n}\n";
    static const char A[] =
        "{\n\tevents {\n\t\tanim \"md6/mods/k/elevator_PLAYER.md6anim\" {\n\t\t\tevent \"ae_sound\" {\n\t\t\t\tframe 18\n\t\t\t}\n\t\t}\n\t}\n"
        "\taliases {\n\t\talias { name \"mods/k/x\" anim \"md6/mods/k/x.md6anim\" }\n\t}\n}\n";
    static const char B[] =   /* the same clip with the hud rows another pack adds, and the same alias */
        "{\n\tevents {\n\t\tanim \"md6/mods/k/elevator_PLAYER.md6anim\" {\n\t\t\tevent \"ae_hideHudForContextPrompt\" {\n\t\t\t\tframe 0\n\t\t\t}\n"
        "\t\t\tevent \"ae_sound\" {\n\t\t\t\tframe 18\n\t\t\t}\n\t\t\tevent \"ae_showHudForContextPrompt\" {\n\t\t\t\tframe 400\n\t\t\t}\n\t\t}\n\t}\n"
        "\taliases {\n\t\talias { name \"mods/k/x\" anim \"md6/mods/k/x.md6anim\" }\n\t}\n}\n";
    merge_tally mt = { 0, 0 };
    size_t len = 0, l2 = 0;
    int added = 0;
    char *t, *u;
    printf("-- registries: body events merged per entry\n");
    /* The named clip the game holds: one block, the game's row first, the new row once. */
    t = sh_modserve_compose_extend_rows(STOCK_BODY, strlen(STOCK_BODY), CLASH, strlen(CLASH), "events;aliases", &len, &added,
                                        merge_cb, &mt);
    CHECK(t && added == 0 && mt.clips == 1 && mt.rows == 1 && count_of(t, "anim \"md6/a.md6anim\"") == 1 &&
          count_of(t, "frame 1\n") == 1 && strstr(t, "frame 2\n") && strstr(t, "frame 1\n") < strstr(t, "frame 2\n"));
    free(t);
    /* Two packs giving one clip: the union of their rows, in row order; the alias once. */
    mt.clips = mt.rows = 0;
    t = sh_modserve_compose_extend_rows(STOCK_BODY, strlen(STOCK_BODY), A, strlen(A), "events;aliases", &len, &added, merge_cb, &mt);
    u = t ? sh_modserve_compose_extend_rows(t, len, B, strlen(B), "events;aliases", &l2, &added, merge_cb, &mt) : NULL;
    CHECK(u && added == 0 && mt.clips == 1 && mt.rows == 2 && count_of(u, "elevator_PLAYER.md6anim\" {") == 1 &&
          count_of(u, "ae_sound") == 1 && strstr(u, "ae_hideHudForContextPrompt") && strstr(u, "ae_showHudForContextPrompt") &&
          strstr(u, "ae_sound") < strstr(u, "ae_hideHudForContextPrompt") && count_of(u, "alias {") == 1);
    free(u);
    free(t);
    /* The plain extend still refuses the clash (other targets keep their rule). */
    CHECK(sh_modserve_compose_extend(STOCK_BODY, strlen(STOCK_BODY), CLASH, strlen(CLASH), "events;aliases", SH_MR_BAG, &len, &added) == NULL);

    /* The converted campaign packs 1 and 2 (23 entries, and 41: 40 clips and
     * one alias; pack 1's clips are all pack 2's, three of them with fewer
     * rows), when the owner's converted folder is present. */
    {
        char p1[MAX_PATH], p2[MAX_PATH];
        char *a = NULL, *b = NULL;
        size_t la = 0, lb = 0;
        _snprintf_s(p1, sizeof p1, _TRUNCATE, "%s/campaign-interactables/campaign-interactables-1/assets/generated/decls/md6def/mods/"
                    "campaign-interactables/body_events.md6.decl", mods ? mods : "");
        _snprintf_s(p2, sizeof p2, _TRUNCATE, "%s/campaign-interactables/campaign-interactables-2/assets/generated/decls/md6def/mods/"
                    "campaign-interactables-2/body_events.md6.decl", mods ? mods : "");
        if (mods && *mods) { a = read_all(p1, &la); b = read_all(p2, &lb); }
        if (!a || !b) {
            printf("   real partials not present (%s); skipped\n", mods && *mods ? mods : "no folder given");
        } else {
            const char *both[2], *one[1];
            int ca, cb, cu, order;
            one[0] = a; ca = distinct_clips(one, 1);
            one[0] = b; cb = distinct_clips(one, 1);
            both[0] = a; both[1] = b; cu = distinct_clips(both, 2);
            printf("   real partials: pack 1 %d clips, pack 2 %d clips, %d distinct\n", ca, cb, cu);
            CHECK(ca == 23 && cb == 40 && count_of(a, "alias {") == 0 && count_of(b, "alias {") == 1);
            for (order = 0; order < 2; ++order) {
                const char *first = order ? b : a, *second = order ? a : b;
                size_t lf = order ? lb : la, ls = order ? la : lb;
                merge_tally m2 = { 0, 0 }, again = { 0, 0 };
                int add1 = 0, add2 = 0, add3 = 0;
                char *x = sh_modserve_compose_extend_rows(STOCK_BODY, strlen(STOCK_BODY), first, lf, "events;aliases", &len, &add1, merge_cb, &m2);
                char *y = x ? sh_modserve_compose_extend_rows(x, len, second, ls, "events;aliases", &l2, &add2, merge_cb, &m2) : NULL;
                char *z1 = NULL, *z2 = NULL;
                size_t lz = 0;
                CHECK(x && y);
                if (!y) { free(x); continue; }
                /* Every clip of both packs once (plus the stand-in game clip). */
                one[0] = y;
                CHECK(distinct_clips(one, 1) == cu + 1 && count_of(y, "\n\t\tanim \"") == cu + 1);
                /* Every row of both is held: composing either pack again adds nothing. */
                z1 = sh_modserve_compose_extend_rows(y, l2, a, la, "events;aliases", &lz, &add3, merge_cb, &again);
                CHECK(z1 && add3 == 0);
                z2 = sh_modserve_compose_extend_rows(y, l2, b, lb, "events;aliases", &lz, &add3, merge_cb, &again);
                CHECK(z2 && add3 == 0 && again.rows == 0);
                printf("   order %s: %d + %d entries appended, %d clip(s) merged with %d row(s)\n", order ? "2,1" : "1,2",
                       add1, add2, m2.clips, m2.rows);
                CHECK(add1 + add2 == cu + 1 && m2.clips == 3 && count_of(y, "alias {") == 1);
                free(z1);
                free(z2);
                free(x);
                free(y);
            }
        }
        free(a);
        free(b);
    }
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

/* What a refresh walk offers: the requests, and the gates of the player entity's. */
typedef struct rf_seen { char requests[4096]; int player_gates; } rf_seen;
static int rf_visit(void *context, const sh_modserve_refresh_item *it)
{
    rf_seen *s = (rf_seen *)context;
    strcat_s(s->requests, sizeof s->requests, it->request);
    strcat_s(s->requests, sizeof s->requests, ";");
    if (!strcmp(it->request, REQ_PLAYER)) s->player_gates = it->gates_live;
    return 0;   /* keep pending */
}

/* A refresh walk that applies what it is offered (the engine's re-parse). */
static int rf_apply(void *context, const sh_modserve_refresh_item *it)
{
    rf_seen *s = (rf_seen *)context;
    strcat_s(s->requests, sizeof s->requests, it->request);
    strcat_s(s->requests, sizeof s->requests, it->gone ? "(game);" : ";");
    return 1;
}

/* "Use in this map" and "Remove from map" on the open map: the generated
 * player entity and weapon slot are pending at once and offered to the
 * refresh, so the scope apply re-parses them in place (it skipped the
 * refresh while only served files counted as pending). */
static void test_map_add_registries(void)
{
    char root[MAX_PATH], hh[65];
    static char text[65536];
    const char *list[1];
    sh_modserve_diff d;
    rf_seen seen;
    sh_mb_stock_ops ops;
    printf("-- registries: map add and remove bring the generated registries in and out at once\n");
    memset(&ops, 0, sizeof ops);
    ops.identify = st_identify;
    ops.read = st_read;
    ops.verify = st_verify;
    sh_mb_set_stock_ops(&ops);
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\set-a\\mods", g_top);
    hash_of('4', hh);
    sh_modserve_scope_enable(1);
    CHECK(sh_modserve_index_build(root) == 5);
    /* An open map that declares nothing; whatever the table brought is settled. */
    CHECK(sh_modserve_scope_set(NULL, 0, NULL, 0, 1) >= 0);
    memset(&seen, 0, sizeof seen);
    (void)sh_modserve_each_refresh(rf_apply, &seen);
    sh_modserve_last_diff(&d);
    CHECK(d.pending == 0 && open_text(REQ_PLAYER, text, sizeof text) <= 0);
    /* Map add of the hands mod: player/coop and the pistol slot are generated, pending and offered. */
    list[0] = hh;
    CHECK(sh_modserve_scope_set(list, 1, NULL, 0, 1) >= 1);
    sh_modserve_last_diff(&d);
    CHECK(d.pending >= 2);
    memset(&seen, 0, sizeof seen);
    (void)sh_modserve_each_refresh(rf_apply, &seen);
    CHECK(strstr(seen.requests, REQ_PLAYER ";") && strstr(seen.requests, REQ_PISTOL ";"));
    CHECK(open_text(REQ_PLAYER, text, sizeof text) == 1 && strstr(text, "thirdPersonBodyMPUseDoomHands = \"mods/k.hands/body\";"));
    sh_modserve_last_diff(&d);
    CHECK(d.pending == 0);
    {
        char got[2][SH_MODSERVE_HASH_HEX + 1];
        CHECK(sh_modserve_scope_hashes(got, 2) == 1 && !strcmp(got[0], hh));
    }
    /* Map remove: back to the game's texts, pending and offered as the game's. */
    CHECK(sh_modserve_scope_set(NULL, 0, NULL, 0, 1) >= 1);
    sh_modserve_last_diff(&d);
    CHECK(d.pending >= 2);
    memset(&seen, 0, sizeof seen);
    (void)sh_modserve_each_refresh(rf_apply, &seen);
    printf("   offered after remove: %s\n", seen.requests);
    CHECK(strstr(seen.requests, REQ_PLAYER "(game);") && strstr(seen.requests, REQ_PISTOL "(game);"));
    CHECK(open_text(REQ_PLAYER, text, sizeof text) <= 0 && !sh_modserve_lookup(REQ_PISTOL, NULL));
    CHECK(sh_modserve_scope_hashes(NULL, 0) == 0);
    CHECK(sh_modserve_scope_set(NULL, 0, NULL, 0, 0) >= 0);
    sh_modserve_scope_enable(0);
    sh_mb_set_stock_ops(NULL);
}

/* "Remove from map" of a converted boss pack (its names under its key, a
 * Toybox row, an encounter, its conductor entries as an extends partial and
 * the one replacement it declares): at once its replacement and its conductor
 * entries go back to the game's (pending, offered as the game's), its names
 * keep serving the editor's Toybox, and in Play every path of it falls
 * through to the game. */
#define RM_GIBS  "generated/decls/entitydef/mods/k.spider/breakable/gibs.decl"
#define RM_ENC   "generated/decls/snapaiencounter/mods/k.spider/single_spider.decl"
#define RM_ROW   "generated/decls/snapeditorentitydef/mods/k.spider/spider.decl"
#define RM_PART  "generated/decls/entitydef/mods/k.spider/conductor_entries.decl"
static void test_map_remove_withdraws(void)
{
    static const fx spider[] = {
        { RM_GIBS, "entityDef", "mods/k.spider/breakable/gibs", "{ inherit = \"breakable/base\"; }" },
        { RM_ENC, "snapAiEncounter", "mods/k.spider/single_spider",
          "{\n\tedit = {\n\t\tmemberList = {\n\t\t\tnum = 1;\n\t\t\titem[0] = {\n\t\t\t\tmemberType = \"1048576\";\n\t\t\t}\n\t\t}\n\t}\n}" },
        { RM_ROW, "snapEditorEntityDef", "mods/k.spider/spider", "{ edit = { entityDef = \"mods/k.spider/breakable/gibs\"; } }" },
        { RM_PART, "entityDef", "mods/k.spider/conductor_entries",
          "{\n\tedit = {\n\t\taiTypeList = {\n\t\t\tnum = 1;\n\t\t\titem[0] = {\n\t\t\t\taiType = \"1048576\";\n"
          "\t\t\t\tentityDef = \"mods/k.spider/breakable/gibs\";\n\t\t\t}\n\t\t}\n\t}\n}" },
        { REQ_SPIDER, "aiFSMManager", "states/spidermastermind", "{\n\tedit = {\n\t\tsubGraphs = {\n\t\t}\n\t}\n}\n" },
    };
    char root[MAX_PATH], hk[65];
    static char text[65536];
    const char *list[1];
    sh_modserve_diff d;
    rf_seen seen;
    sh_mb_stock_ops ops;
    printf("-- registries: map remove withdraws a pack at once; in Play its paths fall through to the game\n");
    memset(&ops, 0, sizeof ops);
    ops.identify = st_identify;
    ops.read = st_read;
    ops.verify = st_verify;
    sh_mb_set_stock_ops(&ops);
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\set-r\\mods", g_top);
    mkdirs(root);
    make_mod(root, "k.spider", "k.spider", '7', NULL,
             "{\"extends\": {\"entitydef/ai/conductor/coop/snapmap_default\": \"mods/k.spider/conductor_entries\"}, "
             "\"replaces\": [\"" REQ_SPIDER "\"]}", "\"" REQ_SPIDER "\"", spider, 5, 0);
    hash_of('7', hk);
    sh_modserve_scope_enable(1);
    CHECK(sh_modserve_index_build(root) == 1);
    /* Declared: everything it carries serves, the conductor names its demon. */
    list[0] = hk;
    CHECK(sh_modserve_scope_set(list, 1, NULL, 0, 1) >= 1);
    memset(&seen, 0, sizeof seen);
    (void)sh_modserve_each_refresh(rf_apply, &seen);
    CHECK(sh_modserve_lookup(RM_GIBS, NULL) && sh_modserve_lookup(RM_ENC, NULL) && sh_modserve_lookup(REQ_SPIDER, NULL));
    CHECK(open_text(REQ_CONDUCTOR, text, sizeof text) == 1 && strstr(text, "mods/k.spider/breakable/gibs"));
    CHECK(sh_modserve_scope_mode(SH_MODSERVE_MODE_PLAY) >= 0 && sh_modserve_lookup(RM_GIBS, NULL));
    (void)sh_modserve_scope_mode(SH_MODSERVE_MODE_EDITOR);
    /* Removed in the editor: the replacement and the conductor go back to the game's at once. */
    CHECK(sh_modserve_scope_set(NULL, 0, NULL, 0, 1) >= 1);
    sh_modserve_last_diff(&d);
    CHECK(d.pending >= 2);
    memset(&seen, 0, sizeof seen);
    (void)sh_modserve_each_refresh(rf_apply, &seen);
    CHECK(strstr(seen.requests, REQ_SPIDER "(game);") && strstr(seen.requests, REQ_CONDUCTOR "(game);"));
    CHECK(!sh_modserve_lookup(REQ_SPIDER, NULL) && open_text(REQ_CONDUCTOR, text, sizeof text) <= 0);
    CHECK(sh_modserve_scope_hashes(NULL, 0) == 0);
    /* Its names still serve the editor's Toybox (the row and what it names). */
    CHECK(sh_modserve_lookup(RM_ROW, NULL) && sh_modserve_lookup(RM_GIBS, NULL));
    /* The next Play: nothing of it serves; each path falls through to the game. */
    CHECK(sh_modserve_scope_mode(SH_MODSERVE_MODE_PLAY) >= 1);
    CHECK(!sh_modserve_lookup(RM_GIBS, NULL) && !sh_modserve_lookup(RM_ENC, NULL) && !sh_modserve_lookup(RM_ROW, NULL) &&
          !sh_modserve_lookup(RM_PART, NULL) && !sh_modserve_lookup(REQ_SPIDER, NULL) && !sh_modserve_lookup(REQ_CONDUCTOR, NULL));
    {
        unsigned char *bytes = NULL;
        size_t len = 0;
        CHECK(sh_modserve_open(RM_GIBS, &bytes, &len) == 0 && !bytes);
    }
    (void)sh_modserve_scope_mode(SH_MODSERVE_MODE_EDITOR);
    CHECK(sh_modserve_scope_set(NULL, 0, NULL, 0, 0) >= 0);
    sh_modserve_scope_enable(0);
    sh_mb_set_stock_ops(NULL);
}

/* L2b's leftovers: a rescan marks changed registry text, generated text is
 * gated by its sources' requirements, and a carried copy of a non-fixed
 * extends target is the base its partials extend. */
static void test_leftovers(void)
{
    static const fx body1[] = {
        { "generated/decls/md6def/mods/k.edit/body_events.md6.decl", "md6Def", "mods/k.edit/body_events.md6",
          "{\n\tevents {\n\t\tanim \"md6/mods/k.edit/one.md6anim\" {\n\t\t\tevent \"ae_hide\" {\n\t\t\t\tframe 1\n\t\t\t}\n\t\t}\n\t}\n}\n" },
    };
    static const fx body2[] = {
        { "generated/decls/md6def/mods/k.edit/body_events.md6.decl", "md6Def", "mods/k.edit/body_events.md6",
          "{\n\tevents {\n\t\tanim \"md6/mods/k.edit/one.md6anim\" {\n\t\t\tevent \"ae_hide\" {\n\t\t\t\tframe 1\n\t\t\t}\n\t\t}\n"
          "\t\tanim \"md6/mods/k.edit/two.md6anim\" {\n\t\t\tevent \"ae_show\" {\n\t\t\t\tframe 2\n\t\t\t}\n\t\t}\n\t}\n}\n" },
    };
    static const fx gated[] = {
        { "generated/decls/entitydef/mods/k.gated/body.decl", "entityDef", "mods/k.gated/body", "{ inherit = \"player_tp_body_mp\"; }" },
    };
    static const fx copy[] = {   /* a pack built before extends: its own copy of the spider states, one graph added */
        { "generated/decls/aifsmmanager/states/spidermastermind.decl", "aiFSMManager", "states/spidermastermind",
          "{\n\tedit = {\n\t\tobject = {\n\t\t\tdebugFlags = 6;\n\t\t}\n\t\tsubGraphs = {\n\t\t\tsubGraph = {\n\t\t\t\tobject = {\n"
          "\t\t\t\t\tname = \"spiderMastermind\";\n\t\t\t\t}\n\t\t\t}\n\t\t\tsubGraph = {\n\t\t\t\tobject = {\n"
          "\t\t\t\t\tname = \"legacy_root\";\n\t\t\t\t}\n\t\t\t}\n\t\t}\n\t}\n}\n" },
    };
    static const fx part[] = {
        { "generated/decls/aifsmmanager/mods/k.ext/more.decl", "aiFSMManager", "mods/k.ext/more",
          "{\n\tedit = {\n\t\tsubGraphs = {\n\t\t\tsubGraph = {\n\t\t\t\tobject = {\n\t\t\t\t\tname = \"ext_root\";\n\t\t\t\t}\n\t\t\t}\n\t\t}\n\t}\n}\n" },
    };
    static char text[65536];
    char root[MAX_PATH], he[65], hg[65], hc[65], hx[65];
    const char *list[3];
    sh_mb_stock_ops ops;
    sh_modserve_diff d;
    rf_seen seen;
    printf("-- registries: rescan marks, gates of generated text, carried copies of extends targets\n");
    memset(&ops, 0, sizeof ops);
    ops.identify = st_identify;
    ops.read = st_read;
    ops.verify = st_verify;
    sh_mb_set_stock_ops(&ops);
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\set-left\\mods", g_top);
    mkdirs(root);
    make_mod(root, "edit", "k.edit", '6', "\"player\": {\"bodyEvents\": \"mods/k.edit/body_events.md6\"},", NULL, NULL, body1, 1, 0);
    make_mod(root, "gated", "k.gated", '7',
             "\"requirements\": {\"cvars\": {\"g_weaponDoomMPUseDoomHands\": \"1\"}}, \"player\": {\"body\": \"mods/k.gated/body\"},",
             NULL, NULL, gated, 1, 0);
    make_mod(root, "copy", "k.copy", '8', NULL, NULL, "\"generated/decls/aifsmmanager/states/spidermastermind.decl\"", copy, 1, 1);
    make_mod(root, "ext", "k.ext", '9', NULL, "{\"extends\": {\"aifsmmanager/states/spidermastermind\": \"mods/k.ext/more\"}}",
             NULL, part, 1, 0);
    hash_of('6', he); hash_of('7', hg); hash_of('8', hc); hash_of('9', hx);
    sh_modserve_scope_enable(1);
    CHECK(sh_modserve_index_build(root) == 4);
    list[0] = he; list[1] = hg;
    CHECK(sh_modserve_scope_set(list, 2, NULL, 0, 1) >= 1);
    CHECK(open_text(REQ_BODY, text, sizeof text) == 1 && strstr(text, "md6/mods/k.edit/one.md6anim") && !strstr(text, "two.md6anim"));
    CHECK(open_text(REQ_PLAYER, text, sizeof text) == 1 && strstr(text, "mods/k.gated/body"));
    /* The author edits the partial; the rescan marks the body for refresh. */
    make_mod(root, "edit", "k.edit", '6', "\"player\": {\"bodyEvents\": \"mods/k.edit/body_events.md6\"},", NULL, NULL, body2, 1, 0);
    CHECK(sh_modserve_index_build(root) == 4);
    sh_modserve_last_diff(&d);
    CHECK(d.changed_decls >= 1);
    CHECK(open_text(REQ_BODY, text, sizeof text) == 1 && strstr(text, "two.md6anim"));
    memset(&seen, 0, sizeof seen);
    seen.player_gates = -1;
    (void)sh_modserve_each_refresh(rf_visit, &seen);
    CHECK(strstr(seen.requests, REQ_BODY ";") != NULL || (printf("  refresh: %s\n", seen.requests), 0));
    /* The player entity's text comes from k.gated, whose cvar is not live: gated as its source. */
    make_mod(root, "gated", "k.gated", '7',
             "\"requirements\": {\"cvars\": {\"g_weaponDoomMPUseDoomHands\": \"1\"}}, \"player\": {\"hands\": \"player/fp_hands.md6\", "
             "\"body\": \"mods/k.gated/body\"},", NULL, NULL, gated, 1, 0);
    CHECK(sh_modserve_index_build(root) == 4);
    memset(&seen, 0, sizeof seen);
    seen.player_gates = -1;
    (void)sh_modserve_each_refresh(rf_visit, &seen);
    CHECK(seen.player_gates == 0 || (printf("  player gates %d: %s\n", seen.player_gates, seen.requests), 0));
    /* A carried copy of a non-fixed extends target is the base the partials extend. */
    list[0] = hc; list[1] = hx;
    CHECK(sh_modserve_scope_set(list, 2, NULL, 0, 1) >= 1);
    CHECK(open_text(REQ_SPIDER, text, sizeof text) == 1 && strstr(text, "legacy_root") && strstr(text, "ext_root") &&
          strstr(text, "spiderMastermind"));
    /* With no partial, the copy itself serves (it was served before registries). */
    list[0] = hc;
    CHECK(sh_modserve_scope_set(list, 1, NULL, 0, 1) >= 1);
    CHECK(open_text(REQ_SPIDER, text, sizeof text) == 1 && strstr(text, "legacy_root") && !strstr(text, "ext_root"));
    CHECK(sh_modserve_scope_set(NULL, 0, NULL, 0, 0) >= 0);
    sh_modmanifest_reset();
    sh_modserve_scope_enable(0);
    sh_mb_set_stock_ops(NULL);
}

int main(int argc, char **argv)
{
    char tmp[MAX_PATH];
    printf("modregistry_test\n");
    GetTempPathA(sizeof tmp, tmp);
    _snprintf_s(g_top, sizeof g_top, _TRUNCATE, "%ssmp-modregistry-%lu", tmp, GetCurrentProcessId());
    mkdirs(g_top);
    test_text();
    test_extends();
    test_body_rows(argc > 1 ? argv[1] : NULL);
    test_generation();
    test_map_add_registries();
    test_map_remove_withdraws();
    test_leftovers();
    printf("%s -- %d checks, %d failed\n", g_failed ? "FAILED" : "ok", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
