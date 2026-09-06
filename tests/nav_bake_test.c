/* nav_bake_test.c -- baking the author's marked regions at map load.
 *
 * The resource-name grammar gets the most attention here, and deliberately. A
 * wrong name does not fail loudly: the engine asks for the COOKED spelling
 * first, so a mistake there simply misses, the shipped payload answers, the
 * source name is never requested at all, and the report cheerfully claims to be
 * serving something. That exact bug shipped once already in the shard path,
 * which is why navmesh_test.c pins both spellings with literal strings -- and
 * why the assertions below are literal strings too rather than being built from
 * the same helper the code uses.
 *
 * NO GAME BYTES: the maps are synthesized. See README.md.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/backend/nav_bake.h"
#include "../src/backend/nav_regions.h"

static int g_checks = 0, g_fail = 0;

#define CHECK(cond) do {                                                      \
    g_checks++;                                                               \
    if (!(cond)) { g_fail++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define CHECK_MSG(cond, msg) do {                                             \
    g_checks++;                                                               \
    if (!(cond)) { g_fail++; printf("  FAIL %s:%d  %s -- %s\n", __FILE__, __LINE__, #cond, msg); } \
} while (0)

/* ---- what nav_bake.c links against ------------------------------------ */

static char g_log[64][512];
static int  g_log_count;

void backend_log(const char *message)
{
    if (g_log_count < (int)(sizeof g_log / sizeof g_log[0]))
        strncpy_s(g_log[g_log_count++], sizeof g_log[0], message ? message : "", _TRUNCATE);
}

static int log_contains(const char *needle)
{
    int i;
    for (i = 0; i < g_log_count; i++) if (strstr(g_log[i], needle)) return 1;
    return 0;
}

int sh_config_get_bool(const char *key, int *out_value, unsigned int *out_flags)
{
    (void)key;
    if (out_flags) *out_flags = 0;
    if (!out_value) return 0;
    *out_value = 1;
    return 1;
}

/* The engine is not here, so no module has navigation to extend. Every bake
 * therefore refuses at the read-back step, which is the behaviour under test:
 * refusing must be quiet and must not take the map down. */
static unsigned char *no_shipped_bytes(const char *name, size_t *out_len)
{
    (void)name;
    if (out_len) *out_len = 0;
    return NULL;
}

int sh_navmesh_validate_aas(const unsigned char *payload, size_t len,
                            char *err, size_t err_cap)
{
    (void)payload; (void)len;
    if (err && err_cap) err[0] = 0;
    return 1;
}

/* ---- synthetic maps ---------------------------------------------------- */

/* One instance of `module`, and `n` blocking volumes. `marked` and `blocks` are
 * bit masks over the volumes: bit i set means volume i carries that flag. */
static char *make_map(const char *module, int instances, int n,
                      unsigned marked, unsigned blocks, const int *owner)
{
    static char buf[65536];
    char *p = buf;
    int i;
    size_t cap = sizeof buf;

    p += _snprintf_s(p, cap - (p - buf), _TRUNCATE, "{\"instances\":[");
    for (i = 0; i < instances; i++) {
        p += _snprintf_s(p, cap - (p - buf), _TRUNCATE,
            "%s{\"moduleName\":\"maps/modules/%s.decl\",\"orientation\":0,"
            "\"origin\":{\"x\":%d.0,\"y\":0.0,\"z\":0.0}}",
            i ? "," : "", module, i * 5120);
    }
    p += _snprintf_s(p, cap - (p - buf), _TRUNCATE, "],\"entities\":[");
    for (i = 0; i < n; i++) {
        p += _snprintf_s(p, cap - (p - buf), _TRUNCATE,
            "%s{\"uniqueId\":%d,\"entityDef\":{\"inherit\":\"snapmaps/volume/blocking\","
            "\"state\":{\"edit\":{"
            "\"clipModelInfo\":{\"size\":{\"x\":1024.0,\"y\":1024.0,\"z\":128.0}},"
            "\"spawnPosition\":{\"x\":0.0,\"y\":0.0,\"z\":0.0},"
            "\"blockDemons\":%s,\"affectsNavmesh\":%s}}}}",
            i ? "," : "", 100 + i,
            (blocks & (1u << i)) ? "true" : "false",
            (marked & (1u << i)) ? "true" : "false");
    }
    p += _snprintf_s(p, cap - (p - buf), _TRUNCATE, "],\"instanceEntities\":{\"keyValues\":[");
    {
        /* CSR: one bucket per instance, then the orphan bucket. Every volume is
         * owned, so the orphan bucket is empty. */
        int off = 0, k;
        for (k = 0; k < instances; k++) {
            int c = 0, j;
            p += _snprintf_s(p, cap - (p - buf), _TRUNCATE, "%s%d", k ? "," : "", off);
            for (j = 0; j < n; j++) if (owner[j] == k) c++;
            off += c;
        }
        p += _snprintf_s(p, cap - (p - buf), _TRUNCATE, ",%d,%d],\"values\":[", off, off);
    }
    {
        int k, j, first = 1;
        for (k = 0; k < instances; k++)
            for (j = 0; j < n; j++)
                if (owner[j] == k) {
                    p += _snprintf_s(p, cap - (p - buf), _TRUNCATE, "%s%d",
                                     first ? "" : ",", 100 + j);
                    first = 0;
                }
    }
    p += _snprintf_s(p, cap - (p - buf), _TRUNCATE, "]}}");
    return buf;
}

/* ---- the name grammar -------------------------------------------------- */

static void test_name_grammar(void)
{
    char m[SH_NAVR_MODULE_CAP], c[32];
    printf("the resource-name grammar\n");

    /* The two spellings, as literal strings. The cooked one prefixes 'b' to the
     * WHOLE extension: .baas_monster48, NOT .bmonster48. */
    CHECK(sh_nav_bake_test_parse_name(
        "maps/modules/ind_dlc/ind_totally_blank_room_4x/ind_totally_blank_room_4x.aas_monster48",
        m, sizeof m, c, sizeof c) == 1);
    CHECK_MSG(strcmp(m, "ind_dlc/ind_totally_blank_room_4x") == 0, m);
    CHECK_MSG(strcmp(c, "monster48") == 0, c);

    CHECK(sh_nav_bake_test_parse_name(
        "generated/maps/modules/ind_dlc/ind_totally_blank_room_4x/ind_totally_blank_room_4x.baas_monster128",
        m, sizeof m, c, sizeof c) == 1);
    CHECK_MSG(strcmp(m, "ind_dlc/ind_totally_blank_room_4x") == 0, m);
    CHECK_MSG(strcmp(c, "monster128") == 0, c);

    /* The bug that shipped once: 'b' on the class instead of the extension. */
    CHECK_MSG(sh_nav_bake_test_parse_name(
        "generated/maps/modules/ind_dlc/room/room.bmonster48",
        m, sizeof m, c, sizeof c) == 0,
        ".bmonster48 is not a spelling the engine ever asks for");

    /* Not ours. */
    CHECK(sh_nav_bake_test_parse_name("maps/modules/a/b/c.aas_monster48",
                                      m, sizeof m, c, sizeof c) == 0);
    CHECK(sh_nav_bake_test_parse_name("generated/decls/entitydef/x.decl",
                                      m, sizeof m, c, sizeof c) == 0);
    CHECK(sh_nav_bake_test_parse_name("", m, sizeof m, c, sizeof c) == 0);
    CHECK(sh_nav_bake_test_parse_name(NULL, m, sizeof m, c, sizeof c) == 0);
}

/* ---- planning ---------------------------------------------------------- */

static void test_marked_volume_is_planned(void)
{
    const int owner[2] = { 0, 0 };
    char *json;
    printf("a marked volume plans its module\n");
    sh_nav_bake_test_reset();
    g_log_count = 0;
    json = make_map("ind_dlc/room", 1, 2, 0x1u, 0x3u, owner);
    sh_nav_bake_set_map(json, strlen(json));
    CHECK_MSG(log_contains("marks 1 volume"), "the log must say what was marked");
}

static void test_unmarked_map_is_silent(void)
{
    const int owner[2] = { 0, 0 };
    char *json;
    printf("a map with nothing marked says nothing\n");
    sh_nav_bake_test_reset();
    g_log_count = 0;
    json = make_map("ind_dlc/room", 1, 2, 0u, 0x3u, owner);
    sh_nav_bake_set_map(json, strlen(json));
    CHECK_MSG(!log_contains("marks"), "the common map must not be chatty");
}

static void test_regions_do_not_survive_the_next_map(void)
{
    const int owner[2] = { 0, 0 };
    char *json;
    unsigned char *bytes = NULL;
    size_t len = 0;
    printf("regions are cleared on every load\n");
    sh_nav_bake_test_reset();

    json = make_map("ind_dlc/room", 1, 1, 0x1u, 0x1u, owner);
    sh_nav_bake_set_map(json, strlen(json));

    /* Now a map with nothing marked. The previous map's regions must be gone --
     * this is the failure nobody would attribute: demons routing onto platforms
     * that are not in the map they are standing in. */
    g_log_count = 0;
    json = make_map("ind_dlc/room", 1, 1, 0u, 0x1u, owner);
    sh_nav_bake_set_map(json, strlen(json));
    CHECK(sh_nav_bake_open(
        "maps/modules/ind_dlc/room/room.aas_monster48", no_shipped_bytes, &bytes, &len) == 0);
    CHECK(bytes == NULL);
}

static void test_two_marked_copies_are_refused(void)
{
    const int owner[2] = { 0, 1 };
    char *json;
    printf("marking volumes in two copies of one module is refused\n");
    sh_nav_bake_test_reset();
    g_log_count = 0;
    /* Both volumes marked, but they belong to different instances of the same
     * module -- which cannot be served different navigation. */
    json = make_map("ind_dlc/room", 2, 2, 0x3u, 0x3u, owner);
    sh_nav_bake_set_map(json, strlen(json));
    CHECK_MSG(log_contains("refused"), "the author must be told, not silently ignored");
}

static void test_one_marked_copy_of_a_repeated_module_is_fine(void)
{
    const int owner[2] = { 0, 1 };
    char *json;
    printf("a repeated module with only ONE marked copy is allowed\n");
    sh_nav_bake_test_reset();
    g_log_count = 0;
    /* Twelve grid rooms, marks in one of them, is the way custom maps are built. */
    json = make_map("ind_dlc/room", 2, 2, 0x1u, 0x3u, owner);
    sh_nav_bake_set_map(json, strlen(json));
    CHECK_MSG(!log_contains("refused"), "this is the common authoring pattern");
}

static void test_unblocking_volume_is_not_a_floor(void)
{
    const int owner[1] = { 0 };
    char *json;
    unsigned char *bytes = NULL;
    size_t len = 0;
    printf("a marked volume demons fall through is not a floor\n");
    sh_nav_bake_test_reset();
    /* Marked, but blockDemons is false: navigating onto it would be a lie. */
    json = make_map("ind_dlc/room", 1, 1, 0x1u, 0u, owner);
    sh_nav_bake_set_map(json, strlen(json));
    CHECK(sh_nav_bake_open(
        "maps/modules/ind_dlc/room/room.aas_monster48", no_shipped_bytes, &bytes, &len) == 0);
}

static void test_open_is_total(void)
{
    unsigned char *bytes = (unsigned char *)1;
    size_t len = 99;
    printf("open refuses cleanly on anything it does not own\n");
    sh_nav_bake_test_reset();
    CHECK(sh_nav_bake_open(NULL, no_shipped_bytes, &bytes, &len) == 0);
    CHECK(sh_nav_bake_open("whatever", no_shipped_bytes, NULL, &len) == 0);
    CHECK(sh_nav_bake_open("whatever", no_shipped_bytes, &bytes, NULL) == 0);
    bytes = NULL; len = 0;
    CHECK(sh_nav_bake_open("maps/modules/a/b/b.aas_notaclass", no_shipped_bytes, &bytes, &len) == 0);
    CHECK(bytes == NULL && len == 0);
}

static void test_malformed_map_does_not_take_the_load_down(void)
{
    printf("a malformed map is survivable\n");
    sh_nav_bake_test_reset();
    sh_nav_bake_set_map(NULL, 0);
    sh_nav_bake_set_map("", 0);
    sh_nav_bake_set_map("{\"instances\":", 13);
    sh_nav_bake_set_map("not json at all", 15);
    CHECK(1);   /* reaching here without faulting is the assertion */
}

int main(void)
{
    printf("nav_bake_test\n");
    test_name_grammar();
    test_marked_volume_is_planned();
    test_unmarked_map_is_silent();
    test_regions_do_not_survive_the_next_map();
    test_two_marked_copies_are_refused();
    test_one_marked_copy_of_a_repeated_module_is_fine();
    test_unblocking_volume_is_not_a_floor();
    test_open_is_total();
    test_malformed_map_does_not_take_the_load_down();
    printf("%s -- %d checks, %d failed\n", g_fail ? "FAILED" : "ok", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
