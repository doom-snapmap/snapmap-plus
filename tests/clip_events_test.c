/* clip_events_test.c -- the pure half of clip events (clip_events_rule.c): the namespace tests, the md6Def reader, the
 * build's registry rows with each skipped entry's warning, the per-Play registry and the chain rule on fake models.
 * The engine sites are covered by clip_events_sigs_test and detour_windows_test. */
#include "../src/common/text.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "clip_events.h"

static int g_checks, g_failed;
#define CHECK(x) do { g_checks++; if (!(x)) { g_failed++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)

#define SALT "abcd1234"

/* warnings */

static char g_lines[16][512];
static int g_nlines;

static void collect(void *context, const char *line)
{
    (void)context;
    if (g_nlines < 16) {
        strncpy(g_lines[g_nlines], line, sizeof g_lines[0] - 1);
        g_lines[g_nlines][sizeof g_lines[0] - 1] = 0;
    }
    g_nlines++;
}

static int said(const char *part)
{
    int i;
    for (i = 0; i < g_nlines && i < 16; i++) if (strstr(g_lines[i], part)) return 1;
    return 0;
}

/* the mod's md6Defs */

typedef struct fake_def { const char *name, *text; } fake_def;
static const fake_def *g_defs;
static int g_ndefs;

static int read_def(void *context, const char *name, char **text, size_t *len)
{
    int i;
    (void)context;
    for (i = 0; i < g_ndefs; i++) {
        if (!sh_path_eq(g_defs[i].name, name)) continue;
        if (!g_defs[i].text) return -1;
        *len = strlen(g_defs[i].text);
        *text = (char *)malloc(*len + 1);
        memcpy(*text, g_defs[i].text, *len + 1);
        return 1;
    }
    return 0;
}

static char *build(const fake_def *defs, int ndefs, const char *const *names, int n, int *oom)
{
    g_defs = defs;
    g_ndefs = ndefs;
    g_nlines = 0;
    return sh_ce_build_registry(SALT, names, n, read_def, NULL, collect, NULL, oom);
}

#define KILL "md6/mods/" SALT "/player/kill_front.md6anim"
#define KILL2 "md6/mods/" SALT "/player/kill_back.md6anim"
#define ROWS(clip) "\t\tanim \"" clip "\" {\n\t\t\tevent \"ae_attachCamera\" {\n\t\t\t\tframe 0\n\t\t\t\tstring \"camera\"\n" \
                   "\t\t\t}\n\t\t}\n"
#define DEF(base, body) "{\n\tinit {\n\t\tinherit \"" base "\"\n\t}\n\tevents {\n" body "\t}\n}\n"

static void test_names(void)
{
    CHECK(sh_ce_under_mods("md6/mods/x/y.md6anim"));
    CHECK(sh_ce_under_mods("MD6\\Mods\\x"));
    CHECK(!sh_ce_under_mods("md6/player/human/base/motion/kill.md6anim"));
    CHECK(!sh_ce_under_mods("md6/mod"));
    CHECK(!sh_ce_under_mods(NULL));
    CHECK(sh_ce_clip_of_mod(KILL, SALT));
    CHECK(sh_ce_clip_of_mod("MD6\\MODS\\ABCD1234\\a.md6anim", SALT));
    CHECK(!sh_ce_clip_of_mod("md6/mods/abcd12345/a.md6anim", SALT));   /* a longer salt is another mod */
    CHECK(!sh_ce_clip_of_mod("md6/mods/zzzz9999/a.md6anim", SALT));
    CHECK(!sh_ce_clip_of_mod("md6/mods/" SALT "/", SALT));
    CHECK(!sh_ce_clip_of_mod("md6/player/tp_body.md6anim", SALT));
    CHECK(sh_ce_def_of_mod("mods/" SALT "/player/kill_rows.md6", SALT));
    CHECK(!sh_ce_def_of_mod("player/mp/tp_body_mp.md6", SALT));
    CHECK(sh_path_eq("Mods\\A/B", "mods/a/b"));
    CHECK(!sh_path_eq("mods/a", "mods/ab"));
}

static void test_md6_read(void)
{
    sh_ce_md6 m;
    const char *text = "// rows of the kill\n{\n\tinit {\n\t\tinherit \"player/mp/tp_body_mp.md6\" // the body\n\t}\n"
                       "\tevents {\n" ROWS(KILL) "\t\tanim \"" KILL2 "\" {\n\t\t}\n"
                       "\t\t/* anim \"md6/mods/" SALT "/commented.md6anim\" { event \"x\" { } } */\n"
                       "\t}\n\taliases {\n\t\talias \"a\" { anim \"" KILL2 "\" }\n\t}\n}\n";
    CHECK(sh_ce_md6_read(text, strlen(text), &m));
    CHECK(m.base && !strcmp(m.base, "player/mp/tp_body_mp.md6"));
    CHECK(m.nclips == 1 && !strcmp(m.clips[0], KILL));
    CHECK(m.empty == 1);   /* the block with no event row */
    sh_ce_md6_free(&m);
    /* no init block: no base; an inherit outside init is not the base */
    text = "{\n\tinherit \"player/tp_body.md6\"\n\tevents {\n" ROWS(KILL) "\t}\n}\n";
    CHECK(sh_ce_md6_read(text, strlen(text), &m));
    CHECK(!m.base && m.nclips == 1);
    sh_ce_md6_free(&m);
}

static void test_build(void)
{
    static const fake_def defs[] = {
        { "mods/" SALT "/player/kill_rows.md6", DEF("player/mp/tp_body_mp.md6", ROWS(KILL) ROWS(KILL2)) },
        { "mods/" SALT "/player/no_base.md6", "{\n\tevents {\n" ROWS(KILL) "\t}\n}\n" },
        { "mods/" SALT "/player/no_rows.md6", DEF("player/mp/tp_body_mp.md6", "") },
        { "mods/" SALT "/player/stock_rows.md6",
          DEF("player/mp/tp_body_mp.md6", ROWS("md6/player/human/base/motion/kill.md6anim")
                                          ROWS("md6/mods/zzzz9999/theirs.md6anim")) },
        { "mods/" SALT "/player/again.md6", DEF("player/tp_body.md6", ROWS(KILL)) },
        { "mods/" SALT "/player/locked.md6", NULL },
    };
    const char *one[] = { "mods/" SALT "/player/kill_rows.md6" };
    const char *all[] = { "mods/" SALT "/player/kill_rows.md6", "mods/" SALT "/player/missing.md6",
                          "player/mp/tp_body_mp.md6", "mods/" SALT "/player/no_base.md6",
                          "mods/" SALT "/player/no_rows.md6", "mods/" SALT "/player/stock_rows.md6",
                          "mods/" SALT "/player/again.md6", "MODS/" SALT "/PLAYER/KILL_ROWS.MD6",
                          "mods/" SALT "/player/locked.md6" };
    int oom = 0;
    char *json = build(defs, 6, one, 1, &oom);
    CHECK(!oom && json);
    CHECK(json && !strcmp(json, "[{\"md6Def\":\"mods/" SALT "/player/kill_rows.md6\",\"base\":\"player/mp/tp_body_mp.md6\","
                                "\"clips\":[\"" KILL "\",\"" KILL2 "\"]}]"));
    CHECK(g_nlines == 0);
    free(json);

    json = build(defs, 6, all, 9, &oom);
    CHECK(!oom && json);
    /* only the good entry is recorded; every other is one warning and the build goes on */
    CHECK(json && strstr(json, "kill_rows.md6") && !strstr(json, "again.md6") && !strstr(json, "stock_rows"));
    CHECK(said("\"mods/" SALT "/player/missing.md6\" is not a model definition of this mod, so it was left out"));
    CHECK(said("\"player/mp/tp_body_mp.md6\" is not a model definition of this mod"));
    CHECK(said("\"mods/" SALT "/player/no_base.md6\" does not say which game model it adds rows to, so it was left out. "
               "Start it with init { inherit \"<that model>\" }."));
    CHECK(said("\"mods/" SALT "/player/no_rows.md6\" adds no event rows, so it was left out"));
    CHECK(said("adds rows to \"md6/player/human/base/motion/kill.md6anim\", which is not a clip of this mod"));
    CHECK(said("adds rows to \"md6/mods/zzzz9999/theirs.md6anim\", which is not a clip of this mod"));
    CHECK(said("\"" KILL "\" gets rows from \"mods/" SALT "/player/kill_rows.md6\" and from \"mods/" SALT
               "/player/again.md6\"; only \"mods/" SALT "/player/kill_rows.md6\"'s are used. Keep its rows in one md6Def."));
    CHECK(said("lists \"MODS/" SALT "/PLAYER/KILL_ROWS.MD6\" twice; the second entry is ignored"));
    CHECK(said("\"mods/" SALT "/player/locked.md6\" could not be read"));
    CHECK(g_nlines == 9);
    free(json);

    /* nothing usable: no row, no failure */
    json = build(defs, 6, all + 1, 5, &oom);
    CHECK(!json && !oom);
    CHECK(!build(defs, 6, NULL, 0, &oom) && !oom);
}

static void test_registry(void)
{
    sh_ce_registry r;
    sh_ce_row *w;
    const char *reg = "{\"banks\":[\"x\"],\"clipEvents\":[{\"md6Def\":\"mods/" SALT "/player/kill_rows.md6\","
                      "\"base\":\"player/mp/tp_body_mp.md6\",\"clips\":[\"" KILL2 "\",\"" KILL "\","
                      "\"md6/player/human/stock.md6anim\"]},"
                      "{\"md6Def\":\"player/mp/tp_body_mp.md6\",\"base\":\"player/tp_body.md6\","
                      "\"clips\":[\"md6/mods/" SALT "/other.md6anim\"]}],\"strings\":[]}";
    const char *other = "{\"clipEvents\":[{\"md6Def\":\"mods/zzzz9999/rows.md6\",\"base\":\"player/tp_body.md6\","
                        "\"clips\":[\"md6/mods/zzzz9999/a.md6anim\",\"" KILL "\"]}]}";
    memset(&r, 0, sizeof r);
    g_nlines = 0;
    CHECK(sh_ce_registry_add(&r, SALT, "Kills (mods\\kills)", reg, collect, NULL) == 2);
    /* a stock clip and a carrier that is a game model are never registered, whatever a build says */
    CHECK(g_nlines == 2);
    CHECK(said("adds rows to a clip that is not its own"));
    CHECK(said("is not one of its own md6Defs"));
    /* another mod cannot register this mod's clip */
    CHECK(sh_ce_registry_add(&r, "zzzz9999", NULL, other, collect, NULL) == 1);
    CHECK(sh_ce_registry_add(&r, SALT, NULL, "{\"banks\":[]}", collect, NULL) == 0);
    CHECK(sh_ce_registry_add(&r, SALT, NULL, NULL, collect, NULL) == 0);
    g_nlines = 0;
    CHECK(sh_ce_registry_add(&r, SALT, NULL, "{\"clipEvents\":[", collect, NULL) == 0);
    CHECK(said("its build's record could not be read"));
    sh_ce_registry_seal(&r);
    CHECK(r.n == 3 && r.mods == 2);
    w = sh_ce_registry_find(&r, "MD6\\MODS\\" SALT "\\PLAYER\\KILL_FRONT.MD6ANIM");
    CHECK(w && !strcmp(w->carrier, "mods/" SALT "/player/kill_rows.md6") && !strcmp(w->base, "player/mp/tp_body_mp.md6"));
    CHECK(w && !strcmp(w->label, "Kills (mods\\kills)") && w->state == SH_CE_ROW_NEW && !w->model);
    CHECK(sh_ce_registry_find(&r, KILL2) != NULL);
    w = sh_ce_registry_find(&r, "md6/mods/zzzz9999/a.md6anim");
    CHECK(w && !strcmp(w->salt, "zzzz9999") && !strcmp(w->label, "zzzz9999"));
    CHECK(!sh_ce_registry_find(&r, "md6/player/human/stock.md6anim"));
    CHECK(!sh_ce_registry_find(&r, "md6/mods/" SALT "/player/kill_front"));
    sh_ce_registry_free(&r);
    CHECK(!r.n && !r.rows);
}

/* Fake md6Defs: tp_body <- tp_body_mp <- carrier; tp_body_mp <- a child body; a demon elsewhere; a loop. */
typedef struct fake_model { struct fake_model *parent; int unreadable; } fake_model;

static int parent_of(void *context, const void *model, const void **parent)
{
    const fake_model *m = (const fake_model *)model;
    (void)context;
    if (m->unreadable) return 0;
    *parent = m->parent;
    return 1;
}

static void test_chain(void)
{
    fake_model body = { NULL, 0 }, body_mp = { &body, 0 }, carrier = { &body_mp, 0 }, child = { &body_mp, 0 };
    fake_model grandchild = { &child, 0 }, demon_base = { NULL, 0 }, demon = { &demon_base, 0 };
    fake_model loop_a = { NULL, 0 }, loop_b = { &loop_a, 0 }, broken = { NULL, 1 };
    loop_a.parent = &loop_b;
    CHECK(sh_ce_in_chain(&carrier, &body_mp, parent_of, NULL));      /* the model it inherits */
    CHECK(sh_ce_in_chain(&carrier, &body, parent_of, NULL));         /* that model's own parent */
    CHECK(sh_ce_in_chain(&carrier, &carrier, parent_of, NULL));
    CHECK(sh_ce_in_chain(&carrier, &child, parent_of, NULL));        /* a body below the one it names */
    CHECK(sh_ce_in_chain(&carrier, &grandchild, parent_of, NULL));
    CHECK(!sh_ce_in_chain(&carrier, &demon, parent_of, NULL));       /* nothing else */
    CHECK(!sh_ce_in_chain(&carrier, &demon_base, parent_of, NULL));
    CHECK(!sh_ce_in_chain(&carrier, &loop_a, parent_of, NULL));      /* a loop ends */
    CHECK(!sh_ce_in_chain(&carrier, &broken, parent_of, NULL));
    CHECK(!sh_ce_in_chain(&broken, &body, parent_of, NULL));
    CHECK(!sh_ce_in_chain(NULL, &body, parent_of, NULL));
    CHECK(!sh_ce_in_chain(&carrier, NULL, parent_of, NULL));
}

int main(void)
{
    test_names();
    test_md6_read();
    test_build();
    test_registry();
    test_chain();
    printf("clip_events_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
