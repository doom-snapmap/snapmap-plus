/* modregistry_test.c -- derived edits applied per map (modregistry.c) and
 * their serving (modserve.c, ms_scope_compute): the conductor's rows and bit
 * rewrite, and generation from a synthetic declared set written to a
 * temporary mods folder whose edited copies' edits are derived as the builder
 * derives them, with the game's texts given by a fake installed-content
 * reader: two boss packs' conductor items (bits assigned without collision,
 * the same whatever the install order, encounters and scores served with the
 * assigned bits), the spider's FSM states (a keyed replace and an add), two
 * bodies merged per clip, the player fields of one bundle, a clash with and
 * without the map's choice, a weapon's field only when declared, map add and
 * remove, rescan marks, gates and a whole copy at a generated name. */
#define WIN32_LEAN_AND_MEAN
#include "heap_out.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modregistry.h"
#include "modserve.h"
#include "modmanifest.h"
#include "modbundle.h"
#include "decl_tree.h"

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
    size_t len = 0;
    char *t, rows[1024] = "";
    unsigned from[2] = { 2097152u, 4194304u }, to[2] = { 524288u, 4194304u };
    printf("-- the conductor rows, the spawn-type bit rewrite, the slots\n");
    CHECK(sh_modregistry_conductor_rows(STOCK_CONDUCTOR, strlen(STOCK_CONDUCTOR), row_cb, rows) == 2 &&
          !strcmp(rows, "SPAWN_AI_TYPE_IMP=ai/demon/imp_coop;SPAWN_AI_TYPE_ARCHVILE=ai/demon/archvile_coop;"));
    CHECK(sh_modregistry_conductor_rows("{ edit = { } }", 14, NULL, NULL) == -1);
    /* Bits: memberType and aiType values only, every number in a flag list. */
    {
        static const char RW[] = "{ edit = { memberList = { item[0] = { memberType = \"2097152 SPAWN_AI_TYPE_IMP 4194304\"; "
                                 "numToSpawn = 2097152; } } aiType = \"2097152\"; other = \"2097152\"; } }";
        t = sh_modregistry_rewrite_bits(RW, sizeof RW - 1, from, to, 2, &len);
    }
    CHECK(t && strstr(t, "memberType = \"524288 SPAWN_AI_TYPE_IMP 4194304\"") && strstr(t, "aiType = \"524288\"") &&
          strstr(t, "numToSpawn = 2097152;") && strstr(t, "other = \"2097152\""));
    free(t);
    CHECK(sh_modregistry_rewrite_bits("{ aiType = \"8\"; }", 17, from, to, 2, &len) == NULL);
    CHECK(sh_modregistry_stock_bit(262144u) && sh_modregistry_stock_bit(1u << 30) && !sh_modregistry_stock_bit(524288u) &&
          !sh_modregistry_stock_bit(3u));
    /* Slots: "<game file>/<path>". */
    CHECK(sh_modregistry_slot_valid("entitydef/player/coop/edit/handsModelMPUseDoomHands") &&
          sh_modregistry_slot_valid("aifsmmanager/states/spidermastermind/edit/subGraphs/spiderMastermind") &&
          !sh_modregistry_slot_valid("player.hands") && !sh_modregistry_slot_valid("a/b") &&
          !sh_modregistry_slot_valid("entitydef/player/coop/edit//x") && !sh_modregistry_slot_valid("entitydef/player/coop/a b") &&
          !sh_modregistry_slot_valid("entitydef/player/coop/\"x\""));
}

/* The body's clip count is structural (V2c: 228 for 227): an alias row's anim
 * reference is not a clip, on one line or written across several. */
static void test_body_clip_count(void)
{
    static const char BODY[] =
        "{\n\tinit {\n\t\tinherit \"player/tp_body.md6\"\n\t}\n\tevents {\n"
        "\t\tanim \"md6/a.md6anim\" {\n\t\t\tevent \"ae_x\" {\n\t\t\t\tframe 1\n\t\t\t}\n\t\t}\n"
        "\t\tanim \"md6/b \\\"q\\\".md6anim\"\n\t\t{\n\t\t}\n"
        "\t\t// anim \"md6/commented.md6anim\" {\n"
        "\t}\n\taliases {\n"
        "\t\talias { name \"mods/k0000000/one\" anim \"md6/mods/k0000000/one.md6anim\" }\n"
        "\t\talias {\n\t\t\tname \"mods/k0000000/two\"\n\t\t\tanim \"md6/mods/k0000000/two.md6anim\"\n\t\t}\n"
        "\t}\n\tprops {\n\t\tprop \"x.md6\" {\n\t\t\tanim \"md6/not_a_clip.md6anim\" {\n\t\t\t}\n\t\t}\n\t}\n}\n";
    printf("-- the body's clips counted by structure, not by an alias row's anim word\n");
    CHECK(sh_modregistry_body_clips(BODY, strlen(BODY)) == 2);
    CHECK(sh_modregistry_body_clips("", 0) == 0 && sh_modregistry_body_clips("{ events {", 10) == 0);
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

static void put_json_string(char *out, size_t cap, size_t *at, const char *s)
{
    *at += _snprintf_s(out + *at, cap - *at, _TRUNCATE, "%s", s);
}

/* <root>\<folder>\mod.json with an author block (`author`: members with a
 * trailing comma), a product block and the files. A file at a game name the
 * fake reader holds is an edited copy: its edits are derived here as the
 * builder derives them (sh_dt_diff) and it is listed in shadows, as the
 * builder writes it; `whole` 1 records no edits, so the copy is a shadow
 * served whole. The block is format 2, keyed by the salt `id`. The bundle
 * hash is 64 x `digit`. */
static void make_mod(const char *root, const char *folder, const char *id, char digit, const char *author,
                     const fx *files, int n, int whole)
{
    static char json[262144], edits[131072], shadows[4096];
    char dir[MAX_PATH], path[MAX_PATH], h[65], *p;
    size_t at = 0, ea = 0, sa = 0;
    int i;
    memset(h, digit, 64);
    h[64] = 0;
    edits[0] = shadows[0] = 0;
    for (i = 0; i < n; ++i) {
        const char *stock = stock_text(files[i].request);
        if (!stock) continue;
        sa += _snprintf_s(shadows + sa, sizeof shadows - sa, _TRUNCATE, "%s\"%s\"", sa ? ", " : "", files[i].request);
        if (!whole) {
            char cause[300], *one = sh_dt_diff(files[i].request, stock, strlen(stock), files[i].body, strlen(files[i].body),
                                              NULL, cause, sizeof cause);
            CHECK(one != NULL);
            if (!one) { fprintf(stderr, "  %s: %s\n", files[i].request, cause); continue; }
            ea += _snprintf_s(edits + ea, sizeof edits - ea, _TRUNCATE, "%s%s", ea ? "," : "[", one);
            free(one);
        }
    }
    if (ea) put_json_string(edits, sizeof edits, &ea, "]");
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\%s", root, folder);
    mkdirs(dir);
    at += _snprintf_s(json + at, sizeof json - at, _TRUNCATE,
                      "{\n  \"name\": \"%s\",\n  %s\n"
                      "  \"snapmap-plus\": {\"format\": 2, \"salt\": \"%s\", \"rules\": 8, \"build\": \"%s\", \"built\": %d%s%s,\n"
                      "    \"shadows\": [%s],\n    \"files\": [\n",
                      id, author ? author : "", id, h, 100 + digit, ea ? ", \"edits\": " : "", ea ? edits : "", shadows);
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
    strcat_s((char *)c, 32768, line);
    strcat_s((char *)c, 32768, "\n");
}

static const char *hash_of(char digit, char *out)
{
    memset(out, digit, 64);
    out[64] = 0;
    return out;
}

/* Edited copies the packs carry at the game's names. */
#define BOSS_CONDUCTOR(n, items) \
    "{\r\n\tinherit = \"ai/base/conductor\";\r\n\tedit = {\r\n\t\taiTypeList = {\r\n\t\t\tnum = " n ";\r\n" \
    "\t\t\titem[0] = {\r\n\t\t\t\taiType = \"SPAWN_AI_TYPE_IMP\";\r\n\t\t\t\tentityDef = \"ai/demon/imp_coop\";\r\n\t\t\t}\r\n" \
    "\t\t\titem[1] = {\r\n\t\t\t\taiType = \"SPAWN_AI_TYPE_ARCHVILE\";\r\n\t\t\t\tentityDef = \"ai/demon/archvile_coop\";\r\n\t\t\t}\r\n" \
    items "\t\t}\r\n\t\tglobalAIMods = {\r\n\t\t\tnum = 1;\r\n\t\t\titem[0] = {\r\n\t\t\t\taiType = \"SPAWN_AI_TYPE_IMP\";\r\n\t\t\t}\r\n" \
    "\t\t}\r\n\t}\r\n}"
#define CITEM(n, bit, def) "\t\t\titem[" #n "] = {\r\n\t\t\t\taiType = \"" bit "\";\r\n\t\t\t\tentityDef = \"" def "\";\r\n\t\t\t}\r\n"
#define BODY_WITH(clips, aliases) \
    "{\n\tinit {\n\t\tmesh \"md6/player/body.md6mesh\"\n\t}\n\tevents {\n\t\tanim \"md6/a.md6anim\" {\n" \
    "\t\t\tevent \"ae_x\" {\n\t\t\t\tframe 1\n\t\t\t}\n\t\t}\n" clips "\t}\n\taliases {\n" aliases "\t}\n}\n"
#define PLAYER_WITH(fields) \
    "{\r\n\tinherit = \"player/mp\";\r\n\tedit = {\r\n" fields \
    "\t\tsyncMelee = {\r\n\t\t\tsyncGroups = {\r\n\t\t\t\tnum = 3;\r\n\t\t\t}\r\n\t\t}\r\n\t}\r\n}"
#define SPIDER_ROOT_AND_RENAMED \
    "{\n\tedit = {\n\t\tobject = {\n\t\t\tdebugFlags = 6;\n\t\t}\n\t\tsubGraphs = {\n\t\t\tsubGraph = {\n" \
    "\t\t\t\tobject = {\n\t\t\t\t\tname = \"spiderMastermind\";\n\t\t\t\t\tfsmType = \"idAIFSM\";\n\t\t\t\t}\n\t\t\t}\n" \
    "\t\t\tsubGraph = {\n\t\t\t\tobject = {\n\t\t\t\t\tname = \"spiderMastermind_attacks\";\n\t\t\t\t}\n\t\t\t}\n\t\t}\n\t}\n}\n"

#define ENC(key, name, bit) { "generated/decls/snapaiencounter/mods/" key "/" name ".decl", "snapAiEncounter", "mods/" key "/" name, \
    "{\n\tedit = {\n\t\tmemberList = {\n\t\t\tnum = 1;\n\t\t\titem[0] = {\n\t\t\t\tmemberType = \"" bit "\";\n\t\t\t}\n\t\t}\n\t}\n}" }

static void build_set(const char *root, int reverse)
{
    static const fx guard[] = {
        { REQ_CONDUCTOR, "entityDef", "ai/conductor/coop/snapmap_default",
          BOSS_CONDUCTOR("4", CITEM(2, "2097152", "ai/demon/talismanguard/staff") CITEM(3, "4194304", "ai/demon/talismanguard/hammer")) },
        ENC("localhel", "single_hell_guard_staff", "2097152"),
        ENC("localhel", "single_hell_guard_hammer", "4194304"),
        { "generated/decls/snapaiscore/mods/localhel/hell_guard_staff.decl", "snapAiScore", "mods/localhel/hell_guard_staff",
          "{\n\tedit = {\n\t\taiType = \"2097152\";\n\t\tbaseScore = 1200;\n\t}\n}" },
    };
    static const fx spider[] = {
        { REQ_CONDUCTOR, "entityDef", "ai/conductor/coop/snapmap_default",
          BOSS_CONDUCTOR("3", CITEM(2, "2097152", "ai/demon/spider_mastermind_base")) },
        ENC("localspi", "single_spider_mastermind", "2097152"),
        { REQ_SPIDER, "aiFSMManager", "states/spidermastermind", SPIDER_ROOT_AND_RENAMED },
    };
    static const fx scenes[] = {   /* body clips and an alias; a clip the hands mod adds too, other rows */
        { REQ_BODY, "md6Def", "player/mp/tp_body_mp.md6",
          BODY_WITH("\t\tanim \"md6/mods/kscenes0/a.md6anim\" {\n\t\t\tevent \"ae_hide\" {\n\t\t\t\tframe 1\n\t\t\t}\n\t\t}\n"
                    "\t\tanim \"md6/shared.md6anim\" {\n\t\t\tevent \"ae_scene\" {\n\t\t\t\tframe 2\n\t\t\t}\n\t\t}\n",
                    "\t\talias { name \"mods/kscenes0/x\" anim \"md6/mods/kscenes0/x.md6anim\" }\n") },
    };
    static const fx hands[] = {   /* player fields, a body clip, a weapon's field */
        { REQ_PLAYER, "entityDef", "player/coop",
          PLAYER_WITH("\t\thandsModelMPUseDoomHands = \"player/fp_hands.md6\";\r\n"
                      "\t\tthirdPersonBodyMPUseDoomHands = \"mods/khands00/body\";\r\n") },
        { REQ_BODY, "md6Def", "player/mp/tp_body_mp.md6",
          BODY_WITH("\t\tanim \"md6/mods/khands00/b.md6anim\" {\n\t\t\tevent \"ae_show\" {\n\t\t\t\tframe 4\n\t\t\t}\n\t\t}\n"
                    "\t\tanim \"md6/shared.md6anim\" {\n\t\t\tevent \"ae_hands\" {\n\t\t\t\tframe 9\n\t\t\t}\n\t\t}\n", "") },
        { "generated/decls/entitydef/mods/khands00/body.decl", "entityDef", "mods/khands00/body", "{ inherit = \"player_tp_body_mp\"; }" },
        { REQ_PISTOL, "weapon", "weapon/zion/player/sp/pistol", "{\n\tedit = {\n\t\tdamage = 99;\n\t}\n}\n" },
    };
    static const fx rival[] = {   /* sets the same player field to another value */
        { REQ_PLAYER, "entityDef", "player/coop", PLAYER_WITH("\t\thandsModelMPUseDoomHands = \"mods/krival00/hands.md6\";\r\n") },
    };
    int order[5] = { 0, 1, 2, 3, 4 }, i;
    mkdirs(root);
    for (i = 0; i < 5; ++i) {
        int k0000000 = reverse ? order[4 - i] : order[i];
        if (k0000000 == 0) make_mod(root, reverse ? "z-guard" : "a-guard", "localhel", '1', NULL, guard, 4, 0);
        if (k0000000 == 1) make_mod(root, reverse ? "y-spider" : "b-spider", "localspi", '2', NULL, spider, 3, 0);
        if (k0000000 == 2) make_mod(root, reverse ? "x-scenes" : "c-scenes", "kscenes0", '3', NULL, scenes, 1, 0);
        if (k0000000 == 3) make_mod(root, reverse ? "w-hands" : "d-hands", "khands00", '4', NULL, hands, 4, 0);
        if (k0000000 == 4) make_mod(root, reverse ? "v-rival" : "e-rival", "krival00", '5', NULL, rival, 1, 0);
    }
}

static void conductor_lines(const char *text, char *out)
{
    out[0] = 0;
    (void)sh_modregistry_conductor_rows(text, strlen(text), row_cb, out);
}

static void set_ops(void)
{
    static sh_mb_stock_ops ops;
    memset(&ops, 0, sizeof ops);
    ops.identify = st_identify;
    ops.read = st_read;
    ops.verify = st_verify;
    sh_mb_set_stock_ops(&ops);
}

static void test_generation(void)
{
    static char text[65536], rep[32768], rows1[1024], rows2[1024];
    char root[MAX_PATH], root2[MAX_PATH], hg[65], hs[65], hsc[65], hh[65], hr[65], key[80], line[512];
    const char *list[5];
    sh_modserve_hit hit;
    printf("-- derived edits: generated per map from the declared set\n");
    set_ops();
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\set-a\\mods", g_top);
    _snprintf_s(root2, sizeof root2, _TRUNCATE, "%s\\set-b\\mods", g_top);
    build_set(root, 0);
    build_set(root2, 1);
    hash_of('1', hg); hash_of('2', hs); hash_of('3', hsc); hash_of('4', hh); hash_of('5', hr);

    sh_modserve_scope_enable(1);
    CHECK(sh_modserve_index_build(root) == 5);
    /* No map open, nothing declared: the game's files; an edited copy is never served. */
    CHECK(sh_modserve_scope_set(NULL, 0, 0) >= 0);
    CHECK(open_text(REQ_PLAYER, text, sizeof text) <= 0 && open_text(REQ_CONDUCTOR, text, sizeof text) <= 0);
    CHECK(!sh_modserve_lookup(REQ_PISTOL, &hit));

    /* Both boss packs: their items added, the bits assigned from 1 << 19
     * without collision (both authors wrote 2097152), in row order then by
     * encounter name; the encounters and the score served with them. */
    list[0] = hg; list[1] = hs;
    CHECK(sh_modserve_scope_set(list, 2, 1) >= 2);
    CHECK(open_text(REQ_CONDUCTOR, text, sizeof text) == 1);
    conductor_lines(text, rows1);
    CHECK(!strcmp(rows1, "SPAWN_AI_TYPE_IMP=ai/demon/imp_coop;SPAWN_AI_TYPE_ARCHVILE=ai/demon/archvile_coop;"
                         "1048576=ai/demon/talismanguard/staff;524288=ai/demon/talismanguard/hammer;"
                         "2097152=ai/demon/spider_mastermind_base;") || (printf("%s\n", rows1), 0));
    CHECK(strstr(text, "\t\t\tnum = 5;\r\n") && strstr(text, "globalAIMods = {\r\n\t\t\tnum = 1;"));
    CHECK(sh_modserve_lookup(REQ_CONDUCTOR, &hit) && !strcmp(hit.bundle_id, "snapmap-plus.registries"));
    CHECK(open_text("generated/decls/snapaiencounter/mods/localhel/single_hell_guard_staff.decl", text, sizeof text) == 1 &&
          strstr(text, "memberType = \"1048576\""));
    CHECK(open_text("generated/decls/snapaiencounter/mods/localhel/single_hell_guard_hammer.decl", text, sizeof text) == 1 &&
          strstr(text, "memberType = \"524288\""));
    CHECK(open_text("generated/decls/snapaiscore/mods/localhel/hell_guard_staff.decl", text, sizeof text) == 1 && strstr(text, "aiType = \"1048576\""));
    CHECK(open_text("generated/decls/snapaiencounter/mods/localspi/single_spider_mastermind.decl", text, sizeof text) == 1 &&
          strstr(text, "memberType = \"2097152\""));
    /* The spider's FSM states: its root graph replaces the stock entry of that name, its renamed copy is added. */
    CHECK(open_text(REQ_SPIDER, text, sizeof text) == 1 && strstr(text, "fsmType = \"idAIFSM\"") &&
          strstr(text, "spiderMastermind_attacks") && count_of(text, "name = \"spiderMastermind\"") == 1);
    /* The other row order swaps them, deterministically. */
    list[0] = hs; list[1] = hg;
    CHECK(sh_modserve_scope_set(list, 2, 1) >= 1);
    CHECK(open_text(REQ_CONDUCTOR, text, sizeof text) == 1);
    conductor_lines(text, rows2);
    CHECK(strstr(rows2, "524288=ai/demon/spider_mastermind_base;") != NULL);
    CHECK(open_text("generated/decls/snapaiencounter/mods/localspi/single_spider_mastermind.decl", text, sizeof text) == 1 &&
          strstr(text, "memberType = \"524288\""));
    /* The same manifest on an install in the opposite folder order: the same texts. */
    list[0] = hg; list[1] = hs;
    CHECK(sh_modserve_index_build(root2) == 5);
    CHECK(sh_modserve_scope_set(list, 2, 1) >= 0);
    CHECK(open_text(REQ_CONDUCTOR, text, sizeof text) == 1);
    conductor_lines(text, rows2);
    CHECK(!strcmp(rows1, rows2));
    CHECK(sh_modserve_index_build(root) == 5);

    /* Two bodies merged per clip (a clip both add keeps both rows), the
     * player fields and a weapon field of one bundle. */
    list[0] = hsc; list[1] = hh;
    CHECK(sh_modserve_scope_set(list, 2, 1) >= 2);
    CHECK(open_text(REQ_BODY, text, sizeof text) == 1 && strstr(text, "md6/mods/kscenes0/a.md6anim") &&
          strstr(text, "md6/mods/khands00/b.md6anim") && strstr(text, "anim \"md6/a.md6anim\"") && strstr(text, "mods/kscenes0/x"));
    CHECK(count_of(text, "anim \"md6/shared.md6anim\"") == 1 && strstr(text, "ae_scene") && strstr(text, "ae_hands"));
    CHECK(sh_modregistry_body_clips(text, strlen(text)) == 4);
    CHECK(open_text(REQ_PLAYER, text, sizeof text) == 1 && strstr(text, "handsModelMPUseDoomHands = \"player/fp_hands.md6\";") &&
          strstr(text, "thirdPersonBodyMPUseDoomHands = \"mods/khands00/body\";") && strstr(text, "inherit = \"player/mp\";") &&
          strstr(text, "\t\t\t\tnum = 3;\r\n"));
    CHECK(open_text(REQ_PISTOL, text, sizeof text) == 1 && strstr(text, "damage = 99;"));
    CHECK(sh_modserve_lookup(REQ_PISTOL, &hit) && !strcmp(hit.bundle_id, "snapmap-plus.registries"));
    rep[0] = 0;
    CHECK(sh_modserve_registry_report(report_line, rep) > 0);
    CHECK(strstr(rep, "registry " REQ_PLAYER ": generated") &&
          strstr(rep, "md6def/player/mp/tp_body_mp.md6: 3 edit(s) from kscenes0") &&
          strstr(rep, "player body: the merged body holds 4 clips and 1 alias") &&
          strstr(rep, "registry edits " REQ_PLAYER ": khands00 edits it; its copy is never served"));
    if (!strstr(rep, "player body: the merged body holds 4 clips")) printf("%s\n", rep);
    /* Played: the same (the declared set serves in Play). */
    CHECK(sh_modserve_scope_mode(SH_MODSERVE_MODE_PLAY) >= 0);
    CHECK(open_text(REQ_PISTOL, text, sizeof text) == 1 && strstr(text, "damage = 99;"));
    (void)sh_modserve_scope_mode(SH_MODSERVE_MODE_EDITOR);

    /* The weapon's edits apply only when its bundle is declared. */
    list[0] = hsc;
    CHECK(sh_modserve_scope_set(list, 1, 1) >= 1);
    CHECK(!sh_modserve_lookup(REQ_PISTOL, &hit) && open_text(REQ_PISTOL, text, sizeof text) <= 0);
    CHECK(open_text(REQ_PLAYER, text, sizeof text) <= 0);   /* no declared bundle edits it: the game's */

    /* A clash: row order without a choice, one line naming it; the map's choice wins. */
    list[0] = hh; list[1] = hr;
    g_log_len = 0; g_log[0] = 0;
    CHECK(sh_modserve_scope_set(list, 2, 1) >= 2);
    CHECK(open_text(REQ_PLAYER, text, sizeof text) == 1 && strstr(text, "handsModelMPUseDoomHands = \"player/fp_hands.md6\";"));
    CHECK(log_has("entitydef/player/coop/edit/handsModelMPUseDoomHands: set by khands00, "
                  "krival00; serving khands00 (row order; choose with "
                  "sh_mods choose \"entitydef/player/coop/edit/handsModelMPUseDoomHands\" \"<mod name>\")"));
    CHECK(HO(line, sh_modregistry_choose_command("entitydef/player/coop/edit/handsModelMPUseDoomHands", "nobody", HO_P(0))) == -1 &&
          !strcmp(line, "MODS: choose: no mod named 'nobody' is used by this map."));
    /* The mod is named as the player names it: here by its folder. */
    CHECK((HO(line, sh_modregistry_choose_command("entitydef/player/coop/edit/handsModelMPUseDoomHands", "E-RIVAL", HO_P(0))) >= 1 ||
           HO(line, sh_modregistry_choose_command("entitydef/player/coop/edit/handsModelMPUseDoomHands", "V-RIVAL", HO_P(0))) >= 1) &&
          strstr(line, "this map now uses krival00's value for entitydef/player/coop/edit/handsModelMPUseDoomHands. Save the map"));
    CHECK(open_text(REQ_PLAYER, text, sizeof text) == 1 && strstr(text, "handsModelMPUseDoomHands = \"mods/krival00/hands.md6\";") &&
          strstr(text, "thirdPersonBodyMPUseDoomHands = \"mods/khands00/body\";"));
    CHECK(sh_modregistry_choices_get("entitydef/player/coop/edit/handsModelMPUseDoomHands", key, sizeof key) && !strcmp(key, "krival00"));
    CHECK(HO(line, sh_modregistry_choose_command("player.hands", "krival00", HO_P(0))) == -1 && strstr(line, "is not a setting two mods disagree on"));
    CHECK(HO(line, sh_modregistry_choose_command("entitydef/player/coop/edit/handsModelMPUseDoomHands", "", HO_P(0))) >= 1);
    CHECK(open_text(REQ_PLAYER, text, sizeof text) == 1 && strstr(text, "player/fp_hands.md6"));
    /* The losing source's edit is counted: superseded by row order, then by the map's choice. */
    CHECK(log_has("2 edit(s) from krival00: 0 applied, 1 already held, 1 superseded by khands00 (row order)"));
    CHECK(log_has("3 edit(s) from khands00: 2 applied, 0 already held, 1 superseded by krival00's choice"));

    /* A choice persists: the save writes it into its key's row, a load reads
     * it back, and the next listing applies it. */
    {
        static const char MAPJ[] = "{\"entities\":[],\"variables\":{\"allocCount\":[0,0,0,0,0],\"string\":[]}}";
        sh_modmanifest_row ra, rb;
        char *saved, *loaded;
        size_t slen = 0, llen = 0;
        memset(&ra, 0, sizeof ra);
        memset(&rb, 0, sizeof rb);
        strcpy_s(ra.salt, sizeof ra.salt, "khands00");
        strcpy_s(ra.build, sizeof ra.build, hh);
        strcpy_s(rb.salt, sizeof rb.salt, "krival00");
        strcpy_s(rb.build, sizeof rb.build, hr);
        ra.size = rb.size = 10;
        CHECK(sh_modmanifest_current_attach(&ra) >= 0 && sh_modmanifest_current_attach(&rb) >= 0);
        CHECK(HO(line, sh_modregistry_choose_command("entitydef/player/coop/edit/handsModelMPUseDoomHands", "krival00", HO_P(0))) >= 1 &&
              strstr(line, "Save the map to keep the choice."));
        saved = sh_modmanifest_on_save(MAPJ, sizeof MAPJ - 1, &slen);
        CHECK(saved && strstr(saved, "krival00") && strstr(saved, "choices") &&
              strstr(saved, "entitydef/player/coop/edit/handsModelMPUseDoomHands"));
        /* Another map in between: no choice; row order serves. */
        sh_modmanifest_reset();
        CHECK(!sh_modregistry_choices_get("entitydef/player/coop/edit/handsModelMPUseDoomHands", key, sizeof key));
        CHECK(sh_modserve_scope_set(list, 2, 1) >= 0);
        CHECK(open_text(REQ_PLAYER, text, sizeof text) == 1 && strstr(text, "handsModelMPUseDoomHands = \"player/fp_hands.md6\";"));
        /* The saved map loaded again: its choice is read back and applied. */
        loaded = saved ? sh_modmanifest_prepare_load(saved, slen, &llen) : NULL;
        CHECK(sh_modregistry_choices_get("entitydef/player/coop/edit/handsModelMPUseDoomHands", key, sizeof key) &&
              !strcmp(key, "krival00"));
        CHECK(sh_modserve_scope_set(list, 2, 1) >= 0);
        CHECK(open_text(REQ_PLAYER, text, sizeof text) == 1 &&
              strstr(text, "handsModelMPUseDoomHands = \"mods/krival00/hands.md6\";"));
        if (loaded) HeapFree(GetProcessHeap(), 0, loaded);
        if (saved) HeapFree(GetProcessHeap(), 0, saved);
    }

    CHECK(sh_modserve_scope_set(NULL, 0, 0) >= 0);
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
 * player entity and weapon are pending at once and offered to the refresh;
 * the edited copies never reach the engine. */
static void test_map_add_remove(void)
{
    char root[MAX_PATH], hh[65];
    static char text[65536];
    const char *list[1];
    sh_modserve_diff d;
    rf_seen seen;
    printf("-- derived edits: map add and remove bring the generated files in and out at once\n");
    set_ops();
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\set-a\\mods", g_top);
    hash_of('4', hh);
    sh_modserve_scope_enable(1);
    CHECK(sh_modserve_index_build(root) == 5);
    CHECK(sh_modserve_scope_set(NULL, 0, 1) >= 0);
    memset(&seen, 0, sizeof seen);
    (void)sh_modserve_each_refresh(rf_apply, &seen);
    sh_modserve_last_diff(&d);
    CHECK(d.pending == 0 && open_text(REQ_PLAYER, text, sizeof text) <= 0);
    list[0] = hh;
    CHECK(sh_modserve_scope_set(list, 1, 1) >= 1);
    sh_modserve_last_diff(&d);
    CHECK(d.pending >= 2);
    memset(&seen, 0, sizeof seen);
    (void)sh_modserve_each_refresh(rf_apply, &seen);
    CHECK(strstr(seen.requests, REQ_PLAYER ";") && strstr(seen.requests, REQ_PISTOL ";"));
    sh_modserve_last_diff(&d);
    CHECK(d.pending == 0);
    /* The edited copies: not in the plan, not answered to a lookup or a probe, no conflict. */
    {
        sh_modserve_plan plan;
        int unknown = 0, s, copies = 0;
        CHECK(sh_modserve_plan_bundles(list, 1, &plan, &unknown) > 0);
        for (s = 0; s < plan.count; ++s) copies += !strcmp(plan.steps[s].name, "player/coop") || !strcmp(plan.steps[s].name, "weapon/zion/player/sp/pistol");
        CHECK(copies == 0);
        sh_modserve_plan_free(&plan);
        CHECK(sh_modserve_lookup(REQ_PLAYER, NULL));
        CHECK(!sh_modserve_conflict_info(REQ_PLAYER, NULL) && !sh_modserve_conflict_info(REQ_BODY, NULL));
    }
    /* Map remove: back to the game's texts, pending and offered as the game's. */
    CHECK(sh_modserve_scope_set(NULL, 0, 1) >= 1);
    sh_modserve_last_diff(&d);
    CHECK(d.pending >= 2);
    memset(&seen, 0, sizeof seen);
    (void)sh_modserve_each_refresh(rf_apply, &seen);
    CHECK(strstr(seen.requests, REQ_PLAYER "(game);") && strstr(seen.requests, REQ_PISTOL "(game);"));
    CHECK(open_text(REQ_PLAYER, text, sizeof text) <= 0 && !sh_modserve_lookup(REQ_PISTOL, NULL));
    CHECK(sh_modserve_scope_set(NULL, 0, 0) >= 0);
    sh_modserve_scope_enable(0);
    sh_mb_set_stock_ops(NULL);
}

/* A rescan marks changed generated text, generated text is gated by its
 * sources' requirements, and a whole copy at a generated name is its mod's
 * edits. */
static void test_leftovers(void)
{
    static const fx body1[] = {
        { REQ_BODY, "md6Def", "player/mp/tp_body_mp.md6",
          BODY_WITH("\t\tanim \"md6/mods/kedit000/one.md6anim\" {\n\t\t\tevent \"ae_hide\" {\n\t\t\t\tframe 1\n\t\t\t}\n\t\t}\n", "") },
    };
    static const fx body2[] = {
        { REQ_BODY, "md6Def", "player/mp/tp_body_mp.md6",
          BODY_WITH("\t\tanim \"md6/mods/kedit000/one.md6anim\" {\n\t\t\tevent \"ae_hide\" {\n\t\t\t\tframe 1\n\t\t\t}\n\t\t}\n"
                    "\t\tanim \"md6/mods/kedit000/two.md6anim\" {\n\t\t\tevent \"ae_show\" {\n\t\t\t\tframe 2\n\t\t\t}\n\t\t}\n", "") },
    };
    static const fx gated[] = {
        { REQ_PLAYER, "entityDef", "player/coop", PLAYER_WITH("\t\tthirdPersonBodyMPUseDoomHands = \"mods/kgated00/body\";\r\n") },
        { "generated/decls/entitydef/mods/kgated00/body.decl", "entityDef", "mods/kgated00/body", "{ inherit = \"player_tp_body_mp\"; }" },
    };
    static const fx gated2[] = {
        { REQ_PLAYER, "entityDef", "player/coop", PLAYER_WITH("\t\tthirdPersonBodyMPUseDoomHands = \"mods/kgated00/body\";\r\n"
                                                             "\t\thandsModelMPUseDoomHands = \"player/fp_hands.md6\";\r\n") },
        { "generated/decls/entitydef/mods/kgated00/body.decl", "entityDef", "mods/kgated00/body", "{ inherit = \"player_tp_body_mp\"; }" },
    };
    static const fx whole[] = {   /* a whole copy of the spider states (a replacement), one graph added */
        { REQ_SPIDER, "aiFSMManager", "states/spidermastermind",
          "{\n\tedit = {\n\t\tobject = {\n\t\t\tdebugFlags = 6;\n\t\t}\n\t\tsubGraphs = {\n\t\t\tsubGraph = {\n\t\t\t\tobject = {\n"
          "\t\t\t\t\tname = \"spiderMastermind\";\n\t\t\t\t}\n\t\t\t}\n\t\t\tsubGraph = {\n\t\t\t\tobject = {\n"
          "\t\t\t\t\tname = \"legacy_root\";\n\t\t\t\t}\n\t\t\t}\n\t\t}\n\t}\n}\n" },
    };
    static const fx adder[] = {   /* edits the same states: one graph added */
        { REQ_SPIDER, "aiFSMManager", "states/spidermastermind",
          "{\n\tedit = {\n\t\tobject = {\n\t\t\tdebugFlags = 6;\n\t\t}\n\t\tsubGraphs = {\n\t\t\tsubGraph = {\n\t\t\t\tobject = {\n"
          "\t\t\t\t\tname = \"spiderMastermind\";\n\t\t\t\t}\n\t\t\t}\n\t\t\tsubGraph = {\n\t\t\t\tobject = {\n"
          "\t\t\t\t\tname = \"ext_root\";\n\t\t\t\t}\n\t\t\t}\n\t\t}\n\t}\n}\n" },
    };
    static char text[65536];
    char root[MAX_PATH], he[65], hg[65], hc[65], hx[65], path[MAX_PATH];
    const char *list[3];
    sh_modserve_diff d;
    rf_seen seen;
    printf("-- derived edits: rescan marks, gates of generated text, whole copies\n");
    set_ops();
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\set-left\\mods", g_top);
    mkdirs(root);
    make_mod(root, "edit", "kedit000", '6', NULL, body1, 1, 0);
    make_mod(root, "gated", "kgated00", '7', "\"requirements\": {\"cvars\": {\"g_weaponDoomMPUseDoomHands\": \"1\"}},", gated, 2, 0);
    make_mod(root, "copy", "kcopy000", '8', NULL, whole, 1, 1);
    make_mod(root, "ext", "kext0000", '9', NULL, adder, 1, 0);
    hash_of('6', he); hash_of('7', hg); hash_of('8', hc); hash_of('9', hx);
    sh_modserve_scope_enable(1);
    CHECK(sh_modserve_index_build(root) == 4);
    list[0] = he; list[1] = hg;
    CHECK(sh_modserve_scope_set(list, 2, 1) >= 1);
    CHECK(open_text(REQ_BODY, text, sizeof text) == 1 && strstr(text, "md6/mods/kedit000/one.md6anim") && !strstr(text, "two.md6anim"));
    CHECK(open_text(REQ_PLAYER, text, sizeof text) == 1 && strstr(text, "mods/kgated00/body"));
    /* The author edits the copy and rebuilds; the rescan marks the body for refresh. */
    make_mod(root, "edit", "kedit000", '6', NULL, body2, 1, 0);
    CHECK(sh_modserve_index_build(root) == 4);
    sh_modserve_last_diff(&d);
    CHECK(d.changed_decls >= 1);
    CHECK(open_text(REQ_BODY, text, sizeof text) == 1 && strstr(text, "two.md6anim"));
    memset(&seen, 0, sizeof seen);
    seen.player_gates = -1;
    (void)sh_modserve_each_refresh(rf_visit, &seen);
    CHECK(strstr(seen.requests, REQ_BODY ";") != NULL || (printf("  refresh: %s\n", seen.requests), 0));
    /* The player entity's text comes from kgated00, whose cvar is not live: gated as its source. */
    make_mod(root, "gated", "kgated00", '7', "\"requirements\": {\"cvars\": {\"g_weaponDoomMPUseDoomHands\": \"1\"}},", gated2, 2, 0);
    CHECK(sh_modserve_index_build(root) == 4);
    memset(&seen, 0, sizeof seen);
    seen.player_gates = -1;
    (void)sh_modserve_each_refresh(rf_visit, &seen);
    CHECK(seen.player_gates == 0 || (printf("  player gates %d: %s\n", seen.player_gates, seen.requests), 0));
    /* A whole copy at a generated name is that mod's edits. */
    list[0] = hc; list[1] = hx;
    CHECK(sh_modserve_scope_set(list, 2, 1) >= 1);
    CHECK(open_text(REQ_SPIDER, text, sizeof text) == 1 && strstr(text, "legacy_root") && strstr(text, "ext_root") &&
          strstr(text, "spiderMastermind"));
    /* Alone, the whole copy serves as it is (no bundle edits that file). */
    list[0] = hc;
    CHECK(sh_modserve_scope_set(list, 1, 1) >= 1);
    CHECK(open_text(REQ_SPIDER, text, sizeof text) == 1 && strstr(text, "legacy_root") && !strstr(text, "ext_root"));
    CHECK(sh_modserve_scope_set(NULL, 0, 0) >= 0);
    sh_modmanifest_reset();
    sh_modserve_scope_enable(0);
    sh_mb_set_stock_ops(NULL);
}

int main(int argc, char **argv)
{
    char tmp[MAX_PATH];
    (void)argc; (void)argv;
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("modregistry_test\n");
    GetTempPathA(sizeof tmp, tmp);
    _snprintf_s(g_top, sizeof g_top, _TRUNCATE, "%ssmp-modregistry-%lu", tmp, GetCurrentProcessId());
    mkdirs(g_top);
    test_text();
    test_body_clip_count();
    test_generation();
    test_map_add_remove();
    test_leftovers();
    printf("%s -- %d checks, %d failed\n", g_failed ? "FAILED" : "ok", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
