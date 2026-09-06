/* nav_traversal_test.c -- the universal traversal table and the animation pick.
 *
 * Three things here are worth more than the rest.
 *
 * The DECL NAME is pinned as a literal string rather than being built from the
 * macro the code uses, for the same reason navmesh_test.c pins its two
 * spellings: a wrong resource name fails SILENTLY. The open simply misses, the
 * shipped data answers, and nothing anywhere says so.
 *
 * The TRAVEL FLAG derivation is checked against all nine distinct traversal
 * flag words that occur anywhere in shipped data. One formula reproducing nine
 * independently observed values is the whole basis for believing it, so if it
 * ever stops doing that the formula is wrong -- not the expectations.
 *
 * The decl is the PLAYER'S file, read through the same hook a package can
 * shadow, so a truncated or hostile one must fail closed and must not be read
 * past. The last test puts a table at the very end of a committed page with the
 * next page unmapped, so a single byte of overrun is an access violation.
 *
 * NO GAME BYTES: every table below is synthesized. See README.md.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include "../src/backend/nav_traversal.h"

static int g_checks = 0, g_fail = 0;

#define CHECK(cond) do {                                                      \
    g_checks++;                                                               \
    if (!(cond)) { g_fail++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define CHECK_MSG(cond, msg) do {                                             \
    g_checks++;                                                               \
    if (!(cond)) { g_fail++; printf("  FAIL %s:%d  %s -- %s\n", __FILE__, __LINE__, #cond, msg); } \
} while (0)

/* ---- writing a decl ---------------------------------------------------- */

/* The shipped file is machine-generated idDeclFile: one assignment per line,
 * every brace on its own line, CRLF endings, and NO trailing newline -- its
 * last byte is '}'. The writer below reproduces all of that, because each of
 * those is something a parser can get wrong. */

static char   g_text[65536];
static size_t g_len;

static void dcl_reset(void) { g_text[0] = 0; g_len = 0; }

static void dcl(int indent, const char *fmt, ...)
{
    va_list ap;
    int n;
    while (indent-- > 0 && g_len + 1 < sizeof g_text) g_text[g_len++] = '\t';
    va_start(ap, fmt);
    n = _vsnprintf_s(g_text + g_len, sizeof g_text - g_len, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n > 0) g_len += (size_t)n;
    if (g_len + 2 < sizeof g_text) { g_text[g_len++] = '\r'; g_text[g_len++] = '\n'; }
    g_text[g_len] = 0;
}

/* Drop the trailing CRLF, so the buffer ends on '}' the way the real one does. */
static void dcl_end(void)
{
    while (g_len && (g_text[g_len - 1] == '\r' || g_text[g_len - 1] == '\n')) g_len--;
    g_text[g_len] = 0;
}

static int g_monster_index;
static int g_row_index;

static void dcl_begin(void)
{
    dcl_reset();
    g_monster_index = 0;
    dcl(0, "{");
    dcl(1, "edit = {");
    dcl(2, "table = {");
}

static void dcl_monster(const char *name)
{
    dcl(3, "table[%d] = {", g_monster_index++);
    dcl(4, "monster = \"%s\";", name);
    dcl(4, "traversal = {");
    g_row_index = 0;
}

/* `offset` carries an x and nothing else -- there is no y and no z anywhere in
 * the shipped file -- and 55 of its 300 rows have no offset block at all, so
 * `has_offset` is a real case and not a synthetic one. `x_scale` is emitted
 * AFTER the offset block, which is also how the shipped file writes it. */
static void dcl_row(const char *type, const char *path, int has_offset, double x,
                    int has_scale, double x_scale)
{
    dcl(5, "traversal[%d] = {", g_row_index++);
    dcl(6, "type = \"%s\";", type);
    dcl(6, "path = \"%s\";", path);
    if (has_offset) {
        dcl(6, "offset = {");
        dcl(7, "x = %g;", x);
        dcl(6, "}");
    }
    if (has_scale) dcl(6, "xScale = %.10f;", x_scale);
    dcl(5, "}");
}

static void dcl_monster_end(void)
{
    dcl(4, "}");
    dcl(3, "}");
}

static void dcl_finish(void)
{
    dcl(2, "}");
    dcl(1, "}");
    dcl(0, "}");
    dcl_end();
}

/* ---- the table the selection tests run against ------------------------- */

static const int ALL_D[6] = { 64, 128, 192, 256, 384, 512 };

/* One family for one demon: `real_count` rows carrying a real path, the rest
 * `_` placeholders. The zombie's LEDGE_UP really does stop part way like this. */
static void write_family(const char *folder, const char *family, const char *stem,
                         int real_count, int offset_at_192)
{
    char type[32], path[192];
    int i;
    for (i = 0; i < 6; i++) {
        _snprintf_s(type, sizeof type, _TRUNCATE, "%s_%d", family, ALL_D[i]);
        if (i < real_count)
            _snprintf_s(path, sizeof path, _TRUNCATE,
                        "zion/characters/monsters/%s/traversal/%s%d", folder, stem, ALL_D[i]);
        else
            _snprintf_s(path, sizeof path, _TRUNCATE,
                        "zion/characters/monsters/%s/traversal/_", folder);
        if (offset_at_192 && ALL_D[i] == 192)
            dcl_row(type, path, 1, -33.0, 1, 1.4199999571);   /* xScale after offset */
        else
            dcl_row(type, path, i % 2 == 0, -32.0, 0, 0.0);
    }
}

/* Imp: everything. Zombie: LEDGE_UP stops at 128, exactly as the shipped table
 * has it. Cyberdemon: nothing at all, and it is not a demon we offer anyway.
 * Mancubus: LEDGE_DOWN only, with no LEDGE_UP rows written at all -- the other
 * way a family can be missing. */
static void build_table(void)
{
    dcl_begin();

    dcl_monster("Imp");
    write_family("imp", "LEDGE_UP", "jump_ledge_up_", 6, 1);
    write_family("imp", "LEDGE_DOWN", "jump_ledge_down_", 6, 0);
    dcl_row("LEAP_ACROSS_64", "zion/characters/monsters/imp/traversal/run_over_railing",
            1, -96.0, 0, 0.0);
    dcl_row("SWIM_UP_64", "zion/characters/monsters/imp/traversal/not_a_family", 0, 0.0, 0, 0.0);
    dcl_monster_end();

    dcl_monster("Zombie");
    write_family("zombie", "LEDGE_UP", "climbledgeup", 2, 0);
    write_family("zombie", "LEDGE_DOWN", "fallledgedown", 6, 0);
    dcl_monster_end();

    dcl_monster("Cyberdemon");
    write_family("cyberdemon", "LEDGE_UP", "jump_ledge_up_", 0, 0);
    write_family("cyberdemon", "LEDGE_DOWN", "jump_ledge_down_", 0, 0);
    dcl_monster_end();

    dcl_monster("Mancubus");
    write_family("mancubus", "LEDGE_DOWN", "jump_ledge_down_", 6, 0);
    dcl_monster_end();

    dcl_finish();
}

/* Imp 12 LEDGE + 1 LEAP_ACROSS (the SWIM_UP row is not a family the table has,
 * so it is dropped) + Zombie 2 up and 6 down + Cyberdemon 0 + Mancubus 6. */
#define EXPECTED_ROWS  (13 + 8 + 0 + 6)

static const sh_trav_monster *demon(const char *key)
{
    int i;
    for (i = 0; i < sh_trav_monster_count(); i++) {
        const sh_trav_monster *m = sh_trav_monster_at(i);
        if (m && strcmp(m->key, key) == 0) return m;
    }
    return NULL;
}

/* ---- the pinned name --------------------------------------------------- */

static void test_decl_name_is_pinned(void)
{
    printf("the decl name is pinned as a literal\n");
    CHECK_MSG(strcmp(SH_TRAV_DECL_NAME,
        "generated/decls/universaltraversaltable/universal_traversal_table.decl") == 0,
        "a wrong resource name misses in silence; nothing else would catch it");
}

/* ---- the demons -------------------------------------------------------- */

static void test_the_roster(void)
{
    static const char *const ORDER[] = {
        "imp", "hellified_soldier", "archvile", "revenant", "baron",
        "hellknight", "zombie", "mancubus", "pinky"
    };
    int i;
    printf("the nine demons, in traversalMonsterType_t order\n");
    CHECK(sh_trav_monster_count() == 9);
    for (i = 0; i < 9 && i < sh_trav_monster_count(); i++) {
        const sh_trav_monster *m = sh_trav_monster_at(i);
        CHECK(m != NULL);
        if (m) CHECK_MSG(strcmp(m->key, ORDER[i]) == 0, m->key);
    }
    /* The decl key must match the table's own spelling byte for byte -- it is
     * the only thing joining a demon to its animation paths. */
    CHECK(strcmp(sh_trav_monster_at(1)->decl_name, "Hellified Soldier") == 0);
    CHECK(strcmp(sh_trav_monster_at(1)->key, "hellified_soldier") == 0);
    /* The player is not offered, and the Cyberdemon is not in the enum at all. */
    CHECK(demon("marine") == NULL);
    CHECK(demon("cyberdemon") == NULL);
    CHECK(sh_trav_monster_at(-1) == NULL);
    CHECK(sh_trav_monster_at(9) == NULL);
}

/* Every distinct traversal travel_flags word observed anywhere in shipped data,
 * with the demon its paired animation name identifies. Nine values, one
 * formula: this is the cross-check the derivation rests on. */
static void test_travel_flags_match_shipped_values(void)
{
    static const struct { const char *key; unsigned flags; } OBSERVED[] = {
        { "imp",               0x11810000u },
        { "archvile",          0x12810000u },
        { "baron",             0x13010000u },
        { "hellified_soldier", 0x14810000u },
        { "zombie",            0x16010000u },
        { "revenant",          0x18810000u },
        { "hellknight",        0x19010000u },
        { "mancubus",          0x1A010000u },
        { "pinky",             0x1C010000u }
    };
    int i;
    printf("the flag formula reproduces all nine observed values\n");
    for (i = 0; i < (int)(sizeof OBSERVED / sizeof OBSERVED[0]); i++) {
        const sh_trav_monster *m = demon(OBSERVED[i].key);
        CHECK_MSG(m != NULL, OBSERVED[i].key);
        if (!m) continue;
        CHECK_MSG(m->travel_flags == OBSERVED[i].flags, OBSERVED[i].key);
        CHECK_MSG(m->travel_flags == (unsigned)((0x1000u | (((unsigned)m->mask) << 7) | 1u) << 16),
                  "travel_flags must be derived, not tabulated");
    }
}

static void test_d30(void)
{
    printf("traversalPoint.d30 follows from the same mask\n");
    /* 0x02000000 | (M << 12) | 0x908, with M = 3, 18 and 12. */
    CHECK(demon("imp")->d30 == 0x02003908u);
    CHECK(demon("hellknight")->d30 == 0x02012908u);
    CHECK(demon("zombie")->d30 == 0x0200C908u);
}

/* ---- parsing ----------------------------------------------------------- */

static void test_parses_a_table(void)
{
    char path[SH_TRAV_PATH_CAP];
    float ox = 0.0f;
    int dist = 0, tt = 0;
    printf("a table parses, and the paths come back verbatim\n");
    sh_trav_test_reset();
    CHECK(sh_trav_ready() == 0);
    build_table();
    CHECK(sh_trav_test_parse(g_text, g_len) == 1);
    CHECK(sh_trav_ready() == 1);

    CHECK(sh_trav_select(demon("imp"), SH_TRAV_UP, 192.0f, path, sizeof path,
                         &ox, &dist, &tt) == 1);
    CHECK_MSG(strcmp(path, "zion/characters/monsters/imp/traversal/jump_ledge_up_192") == 0, path);
    CHECK(dist == 192);
    /* The offset survives an xScale written after the offset block. */
    CHECK(ox == -33.0f);

    CHECK(sh_trav_select(demon("zombie"), SH_TRAV_DOWN, 128.0f, path, sizeof path,
                         &ox, &dist, &tt) == 1);
    CHECK_MSG(strcmp(path, "zion/characters/monsters/zombie/traversal/fallledgedown128") == 0, path);
}

static void test_placeholder_rows_are_dropped(void)
{
    char path[SH_TRAV_PATH_CAP];
    int dist = 0, tt = 0;
    printf("a `_` path is a placeholder, not an animation\n");
    sh_trav_test_reset();
    build_table();
    CHECK(sh_trav_test_parse(g_text, g_len) == 1);

    /* Written rows minus the 58-equivalents: the placeholders never become
     * animations, and neither does a family the table does not have. */
    CHECK_MSG(sh_trav_test_row_count() == EXPECTED_ROWS, "placeholder rows must not be kept");

    /* The zombie's LEDGE_UP stops at 128, so asking for 192 gets the 128 clip
     * rather than a path that resolves to nothing. */
    CHECK(sh_trav_select(demon("zombie"), SH_TRAV_UP, 192.0f, path, sizeof path,
                         NULL, &dist, &tt) == 1);
    CHECK(dist == 128);
    CHECK_MSG(strstr(path, "/_") == NULL, path);
    CHECK_MSG(strcmp(path, "zion/characters/monsters/zombie/traversal/climbledgeup128") == 0, path);
}

/* ---- selection --------------------------------------------------------- */

static void test_nearest_nominal_ties_to_the_smaller(void)
{
    const sh_trav_monster *imp;
    char path[SH_TRAV_PATH_CAP];
    int dist = 0, tt = 0, i;
    static const struct { float drop; int want; } CASES[] = {
        {  64.0f,  64 }, { 128.0f, 128 }, { 192.0f, 192 },
        { 256.0f, 256 }, { 384.0f, 384 }, { 512.0f, 512 },
        {  19.0f,  64 },        /* just above a step, still the smallest clip */
        { 100.0f, 128 },        /* |100-128| = 28 beats |100-64| = 36 */
        { 200.0f, 192 },
        {  96.0f,  64 },        /* an exact tie: 32 either way, take the smaller */
        { 160.0f, 128 },        /* and again */
        { 224.0f, 192 },
        { 448.0f, 384 }
    };
    printf("nearest available nominal, ties to the smaller\n");
    sh_trav_test_reset();
    build_table();
    CHECK(sh_trav_test_parse(g_text, g_len) == 1);
    imp = demon("imp");

    for (i = 0; i < (int)(sizeof CASES / sizeof CASES[0]); i++) {
        char msg[64];
        _snprintf_s(msg, sizeof msg, _TRUNCATE, "drop %g", CASES[i].drop);
        CHECK_MSG(sh_trav_select(imp, SH_TRAV_UP, CASES[i].drop, path, sizeof path,
                                 NULL, &dist, &tt) == 1, msg);
        CHECK_MSG(dist == CASES[i].want, msg);
        CHECK_MSG(sh_trav_select(imp, SH_TRAV_DOWN, CASES[i].drop, path, sizeof path,
                                 NULL, &dist, &tt) == 1, msg);
        CHECK_MSG(dist == CASES[i].want, msg);
    }
}

static void test_travel_time_is_the_median_for_the_nominal(void)
{
    static const struct { float drop; int dist; int time; } CASES[] = {
        {  64.0f,  64, 136 }, { 128.0f, 128, 152 }, { 192.0f, 192, 176 },
        { 256.0f, 256, 170 }, { 384.0f, 384, 183 }, { 512.0f, 512, 205 }
    };
    char path[SH_TRAV_PATH_CAP];
    int dist = 0, tt = 0, i;
    printf("travel time is the shipped median for the selected nominal\n");
    sh_trav_test_reset();
    build_table();
    CHECK(sh_trav_test_parse(g_text, g_len) == 1);
    for (i = 0; i < 6; i++) {
        CHECK(sh_trav_select(demon("imp"), SH_TRAV_UP, CASES[i].drop, path, sizeof path,
                             NULL, &dist, &tt) == 1);
        CHECK(dist == CASES[i].dist);
        CHECK(tt == CASES[i].time);
    }
    /* It is keyed on the NOMINAL, not on the drop -- it is a routing cost. */
    CHECK(sh_trav_select(demon("imp"), SH_TRAV_UP, 200.0f, path, sizeof path,
                         NULL, &dist, &tt) == 1);
    CHECK(dist == 192 && tt == 176);
}

static void test_refuses_past_the_stretch_envelope(void)
{
    char path[SH_TRAV_PATH_CAP];
    int dist = 0, tt = 0;
    printf("a drop past the stretch envelope is refused\n");
    sh_trav_test_reset();
    build_table();
    CHECK(sh_trav_test_parse(g_text, g_len) == 1);

    /* Exactly 2.0x the largest clip is the shipped p99 and is allowed; past it
     * is a warp no shipped record has ever asked the engine for. */
    CHECK(sh_trav_select(demon("imp"), SH_TRAV_UP, 1024.0f, path, sizeof path,
                         NULL, &dist, &tt) == 1);
    CHECK(dist == 512);
    CHECK_MSG(sh_trav_select(demon("imp"), SH_TRAV_UP, 1100.0f, path, sizeof path,
                             NULL, &dist, &tt) == 0, "2.15x is past the envelope");
    CHECK(path[0] == 0 && dist == 0 && tt == 0);

    /* The zombie tops out at its 128 clip, so its ceiling is 256 -- the reason
     * a zombie cannot be given a climb the imp beside it can. */
    CHECK(sh_trav_select(demon("zombie"), SH_TRAV_UP, 250.0f, path, sizeof path,
                         NULL, &dist, &tt) == 1);
    CHECK(dist == 128);
    CHECK(sh_trav_select(demon("zombie"), SH_TRAV_UP, 257.0f, path, sizeof path,
                         NULL, &dist, &tt) == 0);
    /* ...but its LEDGE_DOWN goes the whole way, so the refusal is per family. */
    CHECK(sh_trav_select(demon("zombie"), SH_TRAV_DOWN, 500.0f, path, sizeof path,
                         NULL, &dist, &tt) == 1);

    /* And the other end of the same envelope. */
    CHECK(sh_trav_select(demon("imp"), SH_TRAV_UP, 11.0f, path, sizeof path,
                         NULL, &dist, &tt) == 0);
    CHECK(sh_trav_select(demon("imp"), SH_TRAV_UP, 12.0f, path, sizeof path,
                         NULL, &dist, &tt) == 1);
}

static void test_refuses_a_demon_with_no_row_in_the_family(void)
{
    char path[SH_TRAV_PATH_CAP];
    int dist = 0, tt = 0;
    printf("a demon with no animation in that family is refused\n");
    sh_trav_test_reset();
    build_table();
    CHECK(sh_trav_test_parse(g_text, g_len) == 1);

    /* The mancubus block writes no LEDGE_UP rows at all. */
    CHECK(sh_trav_select(demon("mancubus"), SH_TRAV_UP, 64.0f, path, sizeof path,
                         NULL, &dist, &tt) == 0);
    CHECK(path[0] == 0);
    CHECK(sh_trav_select(demon("mancubus"), SH_TRAV_DOWN, 64.0f, path, sizeof path,
                         NULL, &dist, &tt) == 1);

    /* A demon the table never mentions cannot be served either. */
    CHECK(sh_trav_select(demon("baron"), SH_TRAV_UP, 64.0f, path, sizeof path,
                         NULL, &dist, &tt) == 0);
}

static void test_select_is_total(void)
{
    char path[SH_TRAV_PATH_CAP];
    char tiny[8];
    int dist = 0, tt = 0;
    printf("select refuses cleanly on anything it cannot answer\n");
    sh_trav_test_reset();
    build_table();
    CHECK(sh_trav_test_parse(g_text, g_len) == 1);

    CHECK(sh_trav_select(NULL, SH_TRAV_UP, 64.0f, path, sizeof path, NULL, &dist, &tt) == 0);
    CHECK(sh_trav_select(demon("imp"), 7, 64.0f, path, sizeof path, NULL, &dist, &tt) == 0);
    CHECK(sh_trav_select(demon("imp"), SH_TRAV_UP, 0.0f, path, sizeof path, NULL, &dist, &tt) == 0);
    CHECK(sh_trav_select(demon("imp"), SH_TRAV_UP, -64.0f, path, sizeof path, NULL, &dist, &tt) == 0);
    CHECK(sh_trav_select(demon("imp"), SH_TRAV_UP, 1.0e30f, path, sizeof path, NULL, &dist, &tt) == 0);
    /* A truncated animation path is a path to nothing, so a buffer that cannot
     * hold it is a refusal rather than a partial answer. */
    CHECK(sh_trav_select(demon("imp"), SH_TRAV_UP, 64.0f, tiny, sizeof tiny,
                         NULL, &dist, &tt) == 0);
    CHECK(tiny[0] == 0);
    /* Every output is optional. */
    CHECK(sh_trav_select(demon("imp"), SH_TRAV_UP, 64.0f, NULL, 0, NULL, NULL, NULL) == 1);

    /* With no table, nothing is offered at all. */
    sh_trav_test_reset();
    CHECK(sh_trav_ready() == 0);
    CHECK(sh_trav_select(demon("imp"), SH_TRAV_UP, 64.0f, path, sizeof path,
                         NULL, &dist, &tt) == 0);
}

/* ---- failing closed ---------------------------------------------------- */

static void test_malformed_tables_fail_closed(void)
{
    static const char *const JUNK[] = {
        "",
        "not a decl at all",
        "{",
        "}",
        "{ }",
        "monster = \"Imp\";",
        "traversal[0] = {\r\ntype = \"LEDGE_UP_64\";\r\n",
        "{\r\n\tmonster = \"Imp\r\n}"                       /* unterminated string */
    };
    size_t cut;
    int i;
    printf("a malformed or truncated table fails closed\n");

    CHECK(sh_trav_test_parse(NULL, 0) == 0);
    CHECK(sh_trav_ready() == 0);
    for (i = 0; i < (int)(sizeof JUNK / sizeof JUNK[0]); i++) {
        CHECK_MSG(sh_trav_test_parse(JUNK[i], strlen(JUNK[i])) == 0, JUNK[i]);
        CHECK(sh_trav_ready() == 0);
        CHECK(sh_trav_test_row_count() == 0);
    }

    /* Truncation at every quarter of a real table. Half a table looks exactly
     * like a whole one line by line; only the unclosed blocks give it away. */
    build_table();
    for (cut = g_len / 4; cut < g_len; cut += g_len / 4) {
        CHECK_MSG(sh_trav_test_parse(g_text, cut) == 0, "a truncated table is not a table");
        CHECK(sh_trav_ready() == 0);
    }

    /* A table of nothing but placeholders can produce no climb, so it is not a
     * table we accept either -- ready must stay 0 rather than 0 rows loaded. */
    dcl_begin();
    dcl_monster("Imp");
    write_family("imp", "LEDGE_UP", "jump_ledge_up_", 0, 0);
    write_family("imp", "LEDGE_DOWN", "jump_ledge_down_", 0, 0);
    dcl_monster_end();
    dcl_finish();
    CHECK(sh_trav_test_parse(g_text, g_len) == 0);
    CHECK(sh_trav_ready() == 0);

    /* Nor is one that names only demons we do not offer. */
    dcl_begin();
    dcl_monster("Cyberdemon");
    write_family("cyberdemon", "LEDGE_UP", "jump_ledge_up_", 6, 0);
    dcl_monster_end();
    dcl_finish();
    CHECK(sh_trav_test_parse(g_text, g_len) == 0);

    /* More monsters than there are slots is not the file we think it is. */
    dcl_begin();
    for (i = 0; i < SH_TRAV_MAX_MONSTERS + 4; i++) {
        char name[SH_TRAV_NAME_CAP];
        _snprintf_s(name, sizeof name, _TRUNCATE, "Demon%d", i);
        dcl_monster(name);
        write_family("imp", "LEDGE_UP", "jump_ledge_up_", 6, 0);
        dcl_monster_end();
    }
    dcl_finish();
    CHECK(sh_trav_test_parse(g_text, g_len) == 0);
    CHECK(sh_trav_ready() == 0);
}

/* The decl is not NUL-terminated -- the shipped file's last byte is '}' -- so
 * put one at the very end of a committed page with the next page unmapped. A
 * single byte of overrun is an access violation, which the module's own guard
 * turns into a refusal, so the parse simply stops returning 1. */
static int parse_at_page_end(const char *text, size_t n)
{
    SYSTEM_INFO si;
    char *base;
    int ok;
    GetSystemInfo(&si);
    if (n == 0 || n > si.dwPageSize) return -1;
    base = (char *)VirtualAlloc(NULL, (SIZE_T)si.dwPageSize * 2, MEM_RESERVE, PAGE_NOACCESS);
    if (!base) return -1;
    if (!VirtualAlloc(base, si.dwPageSize, MEM_COMMIT, PAGE_READWRITE)) {
        VirtualFree(base, 0, MEM_RELEASE);
        return -1;
    }
    memcpy(base + si.dwPageSize - n, text, n);
    ok = sh_trav_test_parse(base + si.dwPageSize - n, n);
    VirtualFree(base, 0, MEM_RELEASE);
    return ok;
}

static void test_the_buffer_is_never_read_past(void)
{
    char path[SH_TRAV_PATH_CAP];
    int dist = 0, tt = 0;
    printf("the buffer is not NUL-terminated and is never read past\n");

    dcl_begin();
    dcl_monster("Imp");
    dcl_row("LEDGE_UP_64", "zion/characters/monsters/imp/traversal/jump_ledge_up_64",
            1, -20.0, 0, 0.0);
    dcl_row("LEDGE_DOWN_64", "zion/characters/monsters/imp/traversal/jump_ledge_down_64",
            0, 0.0, 0, 0.0);
    dcl_monster_end();
    dcl_finish();

    CHECK_MSG(parse_at_page_end(g_text, g_len) == 1, "reading one byte past would fault here");
    CHECK(sh_trav_test_row_count() == 2);
    CHECK(sh_trav_select(demon("imp"), SH_TRAV_UP, 64.0f, path, sizeof path,
                         NULL, &dist, &tt) == 1);
    CHECK(dist == 64);

    /* And the same table truncated by one byte, still hard against the page. */
    CHECK(parse_at_page_end(g_text, g_len - 1) == 0);
    CHECK(sh_trav_ready() == 0);
}

/* ---- loading through the reader ---------------------------------------- */

static int   g_reads;
static char  g_asked[256];
static int   g_reader_fails;

static unsigned char *reader(const char *name, size_t *out_len)
{
    unsigned char *copy;
    g_reads++;
    strncpy_s(g_asked, sizeof g_asked, name ? name : "", _TRUNCATE);
    if (out_len) *out_len = 0;
    if (g_reader_fails) return NULL;
    build_table();
    copy = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, g_len);
    if (!copy) return NULL;
    memcpy(copy, g_text, g_len);
    if (out_len) *out_len = g_len;
    return copy;
}

static void test_the_table_is_read_once_and_cached(void)
{
    printf("the table is a property of the install, so it is read once\n");
    sh_trav_test_reset();
    g_reads = 0;
    g_asked[0] = 0;
    g_reader_fails = 0;

    CHECK(sh_trav_load(reader) == 1);
    CHECK(g_reads == 1);
    CHECK_MSG(strcmp(g_asked, SH_TRAV_DECL_NAME) == 0, g_asked);
    CHECK(sh_trav_ready() == 1);

    /* It is not a property of the map, so a second map must not re-read it. */
    CHECK(sh_trav_load(reader) == 1);
    CHECK(g_reads == 1);

    /* A reader that answers with nothing leaves the feature off, not half on. */
    sh_trav_test_reset();
    g_reader_fails = 1;
    CHECK(sh_trav_load(reader) == 0);
    CHECK(sh_trav_ready() == 0);
    CHECK(sh_trav_test_row_count() == 0);
    CHECK(sh_trav_load(NULL) == 0);
}

int main(void)
{
    printf("nav_traversal_test\n");
    test_decl_name_is_pinned();
    test_the_roster();
    test_travel_flags_match_shipped_values();
    test_d30();
    test_parses_a_table();
    test_placeholder_rows_are_dropped();
    test_nearest_nominal_ties_to_the_smaller();
    test_travel_time_is_the_median_for_the_nominal();
    test_refuses_past_the_stretch_envelope();
    test_refuses_a_demon_with_no_row_in_the_family();
    test_select_is_total();
    test_malformed_tables_fail_closed();
    test_the_buffer_is_never_read_past();
    test_the_table_is_read_once_and_cached();
    printf("%s -- %d checks, %d failed\n", g_fail ? "FAILED" : "ok", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
