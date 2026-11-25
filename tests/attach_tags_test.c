/* attach_tags_test.c -- the pure half of attachment tags (attach_tags_rule.c): the namespace test, the md6Def props
 * reader, the build's registry rows with each skipped entry's warning, the per-Play registry, and the chain rule
 * reused from clip events on fake models. The engine site is covered by attach_tags_sigs_test and detour_windows_test. */
#include "../src/common/text.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "attach_tags.h"

static int g_checks, g_failed;
#define CHECK(x) do { g_checks++; if (!(x)) { g_failed++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)

#define SALT "abcd1234"

/* warnings */

static char g_lines[16][640];
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
    return sh_at_build_registry(SALT, names, n, read_def, NULL, collect, NULL, oom);
}

#define GUN "mods/" SALT "/weapons/gun.md6"
#define GUN2 "mods/" SALT "/weapons/rifle.md6"
#define PROP(name) "\t\tprop \"" name "\" {\n\t\t\ttag \"hand_r\" {\n\t\t\t\ttrans ( 0.5 -1e-3 2 )\n" \
                   "\t\t\t\trot ( 0 0 0 1 )\n\t\t\t\tparent \"Rhand\"\n\t\t\t}\n\t\t\ttag \"back\" {\n" \
                   "\t\t\t\tparent \"spine\"\n\t\t\t}\n\t\t}\n"
#define DEF(base, body) "{\n\tinit {\n\t\tinherit \"" base "\"\n\t}\n\tprops {\n" body "\t}\n}\n"

static void test_names(void)
{
    CHECK(sh_at_under_mods("mods/x/y.md6"));
    CHECK(sh_at_under_mods("MODS\\x\\y.md6"));
    CHECK(!sh_at_under_mods("player/mp/tp_body_mp.md6"));
    CHECK(!sh_at_under_mods("mod/x"));
    CHECK(!sh_at_under_mods("mods"));
    CHECK(!sh_at_under_mods(NULL));
    CHECK(sh_at_prop_of_mod(GUN, SALT));
    CHECK(sh_at_prop_of_mod("MODS\\ABCD1234\\Gun.md6", SALT));
    CHECK(!sh_at_prop_of_mod("mods/abcd12345/gun.md6", SALT));   /* a longer salt is another mod */
    CHECK(!sh_at_prop_of_mod("mods/zzzz9999/gun.md6", SALT));
    CHECK(!sh_at_prop_of_mod("mods/" SALT "/", SALT));
    CHECK(!sh_at_prop_of_mod("weapons/shotgun/shotgun.md6", SALT));
    CHECK(!sh_at_prop_of_mod(NULL, SALT));
}

static void test_md6_read(void)
{
    sh_at_md6 m;
    const char *text = "// tag info of the gun\n{\n\tinit {\n\t\tinherit \"player/mp/tp_body_mp.md6\" // the body\n\t}\n"
                       "\tprops {\n" PROP(GUN) "\t\tprop \"" GUN2 "\" {\n\t\t}\n"
                       "\t\t/* prop \"mods/" SALT "/commented.md6\" { tag \"x\" { } } */\n"
                       "\t\tprop \"mods/" SALT "/with } brace and tag.md6\" {\n\t\t\ttag \"a\" { parent \"b } c\" }\n\t\t}\n"
                       "\t}\n\tevents {\n\t\tanim \"md6/mods/" SALT "/a.md6anim\" { event \"x\" { } }\n\t}\n}\n";
    CHECK(sh_at_md6_read(text, strlen(text), &m));
    CHECK(m.base && !strcmp(m.base, "player/mp/tp_body_mp.md6"));
    CHECK(m.nprops == 2 && !strcmp(m.props[0], GUN) && !strcmp(m.props[1], "mods/" SALT "/with } brace and tag.md6"));
    CHECK(m.empty == 1);   /* the block with no tag */
    sh_at_md6_free(&m);
    /* no init block: no base; an inherit outside init is not the base; props outside the top-level block, and a
     * tag word in a string or outside a prop, are not tags */
    text = "{\n\tinherit \"player/tp_body.md6\"\n\tprops {\n" PROP(GUN) "\t}\n"
           "\tother {\n\t\tprop \"" GUN2 "\" {\n\t\t\ttag \"t\" { }\n\t\t}\n\t}\n}\n";
    CHECK(sh_at_md6_read(text, strlen(text), &m));
    CHECK(!m.base && m.nprops == 1 && !strcmp(m.props[0], GUN));
    sh_at_md6_free(&m);
    text = "{\n\tprops {\n\t\ttag \"t\" { }\n\t\tprop \"" GUN "\" {\n\t\t\tname \"tag\"\n\t\t}\n\t}\n}\n";
    CHECK(sh_at_md6_read(text, strlen(text), &m));
    CHECK(m.nprops == 0 && m.empty == 1);
    sh_at_md6_free(&m);
    CHECK(sh_at_md6_read(NULL, 0, &m) && !m.base && !m.nprops);
    sh_at_md6_free(&m);
}

static void test_build(void)
{
    static const fake_def defs[] = {
        { "mods/" SALT "/player/tags.md6", DEF("player/mp/tp_body_mp.md6", PROP(GUN) PROP(GUN2)) },
        { "mods/" SALT "/player/no_base.md6", "{\n\tprops {\n" PROP(GUN) "\t}\n}\n" },
        { "mods/" SALT "/player/no_props.md6", DEF("player/mp/tp_body_mp.md6", "") },
        { "mods/" SALT "/player/stock_props.md6",
          DEF("player/mp/tp_body_mp.md6", PROP("weapons/shotgun/shotgun.md6") PROP("mods/zzzz9999/theirs.md6")) },
        { "mods/" SALT "/player/again.md6", DEF("player/tp_body.md6", PROP(GUN)) },
        { "mods/" SALT "/player/locked.md6", NULL },
    };
    const char *one[] = { "mods/" SALT "/player/tags.md6" };
    const char *all[] = { "mods/" SALT "/player/tags.md6", "mods/" SALT "/player/missing.md6",
                          "player/mp/tp_body_mp.md6", "mods/" SALT "/player/no_base.md6",
                          "mods/" SALT "/player/no_props.md6", "mods/" SALT "/player/stock_props.md6",
                          "mods/" SALT "/player/again.md6", "MODS/" SALT "/PLAYER/TAGS.MD6",
                          "mods/" SALT "/player/locked.md6" };
    int oom = 0;
    char *json = build(defs, 6, one, 1, &oom);
    CHECK(!oom && json);
    CHECK(json && !strcmp(json, "[{\"md6Def\":\"mods/" SALT "/player/tags.md6\",\"base\":\"player/mp/tp_body_mp.md6\","
                                "\"props\":[\"" GUN "\",\"" GUN2 "\"]}]"));
    CHECK(g_nlines == 0);
    free(json);

    json = build(defs, 6, all, 9, &oom);
    CHECK(!oom && json);
    /* only the good entry is recorded; every other is one warning and the build goes on */
    CHECK(json && strstr(json, "tags.md6") && !strstr(json, "again.md6") && !strstr(json, "stock_props"));
    CHECK(said("\"mods/" SALT "/player/missing.md6\" is not a model definition of this mod, so it was left out"));
    CHECK(said("\"player/mp/tp_body_mp.md6\" is not a model definition of this mod"));
    CHECK(said("\"mods/" SALT "/player/no_base.md6\" does not say which game model it gives tag info for, so it was "
               "left out. Start it with init { inherit \"<that model>\" }."));
    CHECK(said("\"mods/" SALT "/player/no_props.md6\" gives no tag info, so it was left out"));
    CHECK(said("gives tag info for \"weapons/shotgun/shotgun.md6\", which is not an md6Def of this mod"));
    CHECK(said("gives tag info for \"mods/zzzz9999/theirs.md6\", which is not an md6Def of this mod"));
    CHECK(said("\"" GUN "\" gets tag info from \"mods/" SALT "/player/tags.md6\" and from \"mods/" SALT
               "/player/again.md6\"; only \"mods/" SALT "/player/tags.md6\"'s is used. Keep its tag info in one md6Def."));
    CHECK(said("lists \"MODS/" SALT "/PLAYER/TAGS.MD6\" twice; the second entry is ignored"));
    CHECK(said("\"mods/" SALT "/player/locked.md6\" could not be read"));
    CHECK(g_nlines == 9);
    free(json);

    /* two good entries make two rows, in order */
    {
        static const fake_def two[] = {
            { "mods/" SALT "/player/a.md6", DEF("player/mp/tp_body_mp.md6", PROP(GUN)) },
            { "mods/" SALT "/player/b.md6", DEF("player/tp_body.md6", PROP(GUN2)) },
        };
        const char *both[] = { "mods/" SALT "/player/a.md6", "mods/" SALT "/player/b.md6" };
        json = build(two, 2, both, 2, &oom);
        CHECK(json && !strcmp(json, "[{\"md6Def\":\"mods/" SALT "/player/a.md6\",\"base\":\"player/mp/tp_body_mp.md6\","
                                    "\"props\":[\"" GUN "\"]},{\"md6Def\":\"mods/" SALT "/player/b.md6\","
                                    "\"base\":\"player/tp_body.md6\",\"props\":[\"" GUN2 "\"]}]"));
        CHECK(g_nlines == 0);
        free(json);
    }

    /* nothing usable: no row, no failure */
    json = build(defs, 6, all + 1, 5, &oom);
    CHECK(!json && !oom);
    CHECK(!build(defs, 6, NULL, 0, &oom) && !oom);
}

static void test_registry(void)
{
    sh_at_registry r;
    sh_at_row *w;
    const char *reg = "{\"banks\":[\"x\"],\"attachmentTags\":[{\"md6Def\":\"mods/" SALT "/player/tags.md6\","
                      "\"base\":\"player/mp/tp_body_mp.md6\",\"props\":[\"" GUN2 "\",\"" GUN "\","
                      "\"weapons/shotgun/shotgun.md6\"]},"
                      "{\"md6Def\":\"player/mp/tp_body_mp.md6\",\"base\":\"player/tp_body.md6\","
                      "\"props\":[\"mods/" SALT "/other.md6\"]}],\"strings\":[]}";
    const char *other = "{\"attachmentTags\":[{\"md6Def\":\"mods/zzzz9999/tags.md6\",\"base\":\"player/tp_body.md6\","
                        "\"props\":[\"mods/zzzz9999/a.md6\",\"" GUN "\"]}]}";
    memset(&r, 0, sizeof r);
    g_nlines = 0;
    CHECK(sh_at_registry_add(&r, SALT, "Guns (mods\\guns)", reg, collect, NULL) == 2);
    /* a stock prop and a carrier that is a game model are never registered, whatever a build says */
    CHECK(g_nlines == 2);
    CHECK(said("gives tag info for an md6Def that is not its own"));
    CHECK(said("is not one of its own md6Defs"));
    /* another mod cannot register this mod's prop */
    CHECK(sh_at_registry_add(&r, "zzzz9999", NULL, other, collect, NULL) == 1);
    CHECK(sh_at_registry_add(&r, SALT, NULL, "{\"banks\":[]}", collect, NULL) == 0);
    CHECK(sh_at_registry_add(&r, SALT, NULL, NULL, collect, NULL) == 0);
    g_nlines = 0;
    CHECK(sh_at_registry_add(&r, SALT, NULL, "{\"attachmentTags\":[", collect, NULL) == 0);
    CHECK(said("its build's record could not be read"));
    sh_at_registry_seal(&r);
    CHECK(r.n == 3 && r.mods == 2);
    w = sh_at_registry_find(&r, "MODS\\" SALT "\\WEAPONS\\GUN.MD6");
    CHECK(w && !strcmp(w->carrier, "mods/" SALT "/player/tags.md6") && !strcmp(w->base, "player/mp/tp_body_mp.md6"));
    CHECK(w && !strcmp(w->label, "Guns (mods\\guns)") && w->state == SH_AT_ROW_NEW && !w->model && !w->answered);
    CHECK(sh_at_registry_find(&r, GUN2) != NULL);
    w = sh_at_registry_find(&r, "mods/zzzz9999/a.md6");
    CHECK(w && !strcmp(w->salt, "zzzz9999") && !strcmp(w->label, "zzzz9999"));
    CHECK(!sh_at_registry_find(&r, "weapons/shotgun/shotgun.md6"));
    CHECK(!sh_at_registry_find(&r, "mods/" SALT "/weapons/gu"));
    CHECK(!sh_at_registry_find(&r, NULL));
    sh_at_registry_free(&r);
    CHECK(!r.n && !r.rows);
}

/* Fake md6Defs: tp_body <- tp_body_mp <- carrier; tp_body_mp <- a child body; a demon elsewhere. The chain rule is
 * clip events' (sh_ce_in_chain); the detour hands it the props table's owner. */
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
    fake_model demon = { NULL, 0 }, broken = { NULL, 1 };
    CHECK(sh_ce_in_chain(&carrier, &body_mp, parent_of, NULL));
    CHECK(sh_ce_in_chain(&carrier, &body, parent_of, NULL));
    CHECK(sh_ce_in_chain(&carrier, &child, parent_of, NULL));
    CHECK(!sh_ce_in_chain(&carrier, &demon, parent_of, NULL));
    CHECK(!sh_ce_in_chain(&carrier, &broken, parent_of, NULL));
}

int main(void)
{
    test_names();
    test_md6_read();
    test_build();
    test_registry();
    test_chain();
    printf("attach_tags_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
