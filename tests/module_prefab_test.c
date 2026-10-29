/* Module prefab cut and merge over synthetic idSnapMap JSON (no game needed).
 *
 * The source map has three modules:
 *   0: entities 56 (cap), 57 (door), 60 (relay), portals [56, 57], frame 70 on 57
 *   1: entities 58 (cap), 59 (cap), 61 (blocking box ticked for AI Navigation)
 *   2: entity 62 (cap), 63 (listener)
 *   global: 64 (player start)
 * plus wires 60->61, 61->60, 60->63 (crosses), 64->60 (crosses), entity
 * references 61 -> 60 (inside) and 63 -> 64 (outside), variable references
 * from 60 (number 1) and 63 (boolean 0), and frame entity 70 in module 0. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "module_prefab_map.h"
#include "mp_json.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

#define ENT(uid, cls, inherit, extra) \
    "{\"customIcon\":{\"targetType\":\"idDeclSnapCustomIcon\",\"value\":null,\"~type\":\"|pointer\"},\"displayName\":\"\"," \
    "\"entityDef\":{\"className\":\"" cls "\",\"inherit\":\"" inherit "\",\"name\":\"\",\"state\":{\"edit\":{" extra "}}," \
    "\"targetType\":\"idDeclEntityDef\",\"~type\":\"idDeclEntityDef\",\"~version\":26},\"isMegaModel\":false,\"layerMask\":1," \
    "\"pinned\":true,\"restrictionMask\":0,\"uniqueId\":" #uid ",\"~type\":\"idSnapEntity\"}"

#define INST(name, x, y, z, rot) \
    "{\"difficultyOffset\":0,\"environmentName\":\"snapmap/base\",\"layerMask\":1,\"moduleName\":\"" name "\"," \
    "\"orientation\":" #rot ",\"origin\":{\"x\":" #x ",\"y\":" #y ",\"z\":" #z ",\"~type\":\"idVec3\"}," \
    "\"restrictionMask\":0,\"~type\":\"idSnapInstance\"}"

#define VAR(type, name) "{\"info\":{\"customIcon\":{\"targetType\":\"idDeclSnapCustomIcon\",\"value\":null,\"~type\":\"|pointer\"},\"name\":\"" name "\",\"~type\":\"snapVarInfo_t\"},\"initialValue\":0,\"~type\":\"" type "\"}"

/* refs rows: EntRefs keyed by uid 0..70 (+1 trailing key): 56 [-1], 57 [-1], 58 [-1],
 * 59 [-1], 61 [60], 62 [-1], 63 [64], 64 [-1]. VarRefs: 60 [number 1], 63 [boolean 0, unbound integer]. */
static char *source_map(void)
{
    static const char head[] =
        "{\"customFilterAllocCount\":0,"
        "\"doorsAndCaps\":{\"portalDoors\":[56,57,58,59,62],\"portalFrames\":[-1,70,-1,-1,-1],\"~type\":\"idSnapDoorsAndCaps\"},"
        "\"entities\":["
        ENT(56, "idSnapMapCapEntity", "snapmaps/visblockers/industrial/cap_01", "") ","
        ENT(57, "idInteractable_Obstacle_SnapDoor", "snapmaps/doors/door", "") ","
        ENT(58, "idSnapMapCapEntity", "snapmaps/visblockers/industrial/cap_01", "") ","
        ENT(59, "idSnapMapCapEntity", "snapmaps/visblockers/industrial/cap_01", "") ","
        ENT(60, "idSnapMapRelay", "snapmaps/logic/relay", "\"spawnPosition\":{\"x\":1.5}") ","
        ENT(61, "idVolume_Blocking", "snapmaps/volume/blocking", "\"flags\":{\"noFlood\":true}") ","
        ENT(62, "idSnapMapCapEntity", "snapmaps/visblockers/industrial/cap_01", "") ","
        ENT(63, "idSnapMapListener", "snapmaps/logic/listener", "") ","
        ENT(64, "idSnapMapGameEntity_ComboStart", "player/coop/player_start", "") ","
        ENT(70, "idSnapMapFrame", "snapmaps/doors/frame", "")
        "],"
        "\"instanceEntities\":{\"keyValues\":[0,4,7,9,10],\"values\":[56,57,60,70,58,59,61,62,63,64],\"~type\":\"idIndexMultimap\"},"
        "\"instances\":["
        INST("maps/modules/a.decl", 1024.0, -512.0, 0.0, 0) ","
        INST("maps/modules/b.decl", 1024.0, 1536.0, 0.0, 2) ","
        INST("maps/modules/c.decl", -2048.0, -512.0, 64.0, 5)
        "],";
    char *buf = (char *)malloc(65536), *p;
    int u;
    if (!buf) return NULL;
    strcpy(buf, head);
    p = buf + strlen(buf);
    /* references */
    p += sprintf(p, "\"references\":{\"entityEntRefs\":{\"keyValues\":[");
    {
        int ent_rows[71] = {0}, var_rows[71] = {0}, acc = 0;
        int ent_vals[16], var_vals[16], ne = 0, nv = 0;
        for (u = 0; u <= 70; u++) {
            switch (u) {
            case 56: case 57: case 58: case 59: case 62: case 64: ent_rows[u] = 1; break;
            case 61: ent_rows[u] = 1; break;
            case 63: ent_rows[u] = 1; break;
            default: break;
            }
            if (u == 60) var_rows[u] = 1;
            if (u == 63) var_rows[u] = 2;
        }
        for (u = 0; u <= 70; u++) {
            p += sprintf(p, "%s%d", u ? "," : "", acc);
            acc += ent_rows[u];
            if (ent_rows[u]) ent_vals[ne++] = u == 61 ? 60 : u == 63 ? 64 : -1;
        }
        p += sprintf(p, ",%d,%d],\"values\":[", acc, acc);
        for (u = 0; u < ne; u++) p += sprintf(p, "%s%d", u ? "," : "", ent_vals[u]);
        p += sprintf(p, "],\"~type\":\"idIndexMultimap\"},\"entityVarRefs\":{\"keyValues\":[");
        acc = 0;
        for (u = 0; u <= 70; u++) {
            p += sprintf(p, "%s%d", u ? "," : "", acc);
            acc += var_rows[u];
            if (u == 60) var_vals[nv++] = (1 << 24) | 1;
            if (u == 63) { var_vals[nv++] = (2 << 24) | 0; var_vals[nv++] = 0xFFFFFF; }
        }
        p += sprintf(p, ",%d,%d],\"values\":[", acc, acc);
        for (u = 0; u < nv; u++) p += sprintf(p, "%s%d", u ? "," : "", var_vals[u]);
        p += sprintf(p, "],\"~type\":\"idIndexMultimap\"},\"~type\":\"idSnapReferences\"},");
    }
    p += sprintf(p,
        "\"targets\":{\"connections\":[-61,61,63,-62,60,-65,60],"
        "\"entitySources\":{\"keyValues\":[],\"values\":[],\"~type\":\"idIndexMultimap\"},"
        "\"entityTargets\":{\"keyValues\":[],\"values\":[],\"~type\":\"idIndexMultimap\"},\"~type\":\"idSnapTargets\"},"
        "\"variables\":{\"allocCount\":[2,4,2,0,1,0,0,0,0,0],"
        "\"boolean\":[" VAR("snapVarBoolean_t", "Boolean 1") "],"
        "\"integer\":[" VAR("snapVarInteger_t", "Integer 1") "],"
        "\"number\":[" VAR("snapVarNumber_t", "Number 1") "," VAR("snapVarNumber_t", "Number 2") "],"
        "\"persistentInteger\":[" VAR("snapVarInteger_t", "Persistent Integer 0") "],"
        "\"string\":[" VAR("snapVarString_t", "smp.render.v1") "],"
        "\"~type\":\"idSnapVariables\"},"
        "\"version\":111,\"~type\":\"idSnapMap\",\"~version\":111}");
    return buf;
}

/* A small target map: one module, uids 56,57 (caps), 61 (start), stale row at 62. */
static char *target_map(void)
{
    char *buf = (char *)malloc(65536), *p;
    int u, acc = 0;
    if (!buf) return NULL;
    p = buf;
    p += sprintf(p,
        "{\"doorsAndCaps\":{\"portalDoors\":[56,57],\"portalFrames\":[-1,-1],\"~type\":\"idSnapDoorsAndCaps\"},"
        "\"entities\":["
        ENT(56, "idSnapMapCapEntity", "snapmaps/visblockers/industrial/cap_01", "") ","
        ENT(57, "idSnapMapCapEntity", "snapmaps/visblockers/industrial/cap_03", "") ","
        ENT(61, "idSnapMapGameEntity_ComboStart", "player/coop/player_start", "")
        "],"
        "\"instanceEntities\":{\"keyValues\":[0,3,3],\"values\":[56,57,61],\"~type\":\"idIndexMultimap\"},"
        "\"instances\":[" INST("maps/modules/t.decl", 1280.0, -2944.0, 0.0, 0) "],"
        "\"references\":{\"entityEntRefs\":{\"keyValues\":[");
    for (u = 0; u <= 62; u++) {
        p += sprintf(p, "%s%d", u ? "," : "", acc);
        if (u == 56 || u == 57 || u == 61 || u == 62) acc++;
    }
    p += sprintf(p, ",%d],\"values\":[-1,-1,-1,-1],\"~type\":\"idIndexMultimap\"},\"entityVarRefs\":{\"keyValues\":[", acc);
    for (u = 0; u <= 63; u++) p += sprintf(p, "%s0", u ? "," : "");
    p += sprintf(p,
        "],\"values\":[],\"~type\":\"idIndexMultimap\"},\"~type\":\"idSnapReferences\"},"
        "\"targets\":{\"connections\":[],\"entitySources\":{\"keyValues\":[],\"values\":[],\"~type\":\"idIndexMultimap\"},"
        "\"entityTargets\":{\"keyValues\":[],\"values\":[],\"~type\":\"idIndexMultimap\"},\"~type\":\"idSnapTargets\"},"
        "\"variables\":{\"allocCount\":[1,1,3,0,1,0,0,0,0,0],"
        "\"boolean\":[" VAR("snapVarBoolean_t", "Boolean 1") "," VAR("snapVarBoolean_t", "Boolean 2") "],"
        "\"number\":[],"
        "\"persistentInteger\":[" VAR("snapVarInteger_t", "Persistent Integer 0") "],"
        "\"string\":[" VAR("snapVarString_t", "smp.render.v1") "],"
        "\"~type\":\"idSnapVariables\"},"
        "\"version\":111,\"~type\":\"idSnapMap\",\"~version\":111}");
    return buf;
}

static void ints_of(const mpj *a, long long *out, int cap, int *n)
{
    int i;
    *n = 0;
    if (!a || a->type != MPJ_ARR) return;
    for (i = 0; i < a->n && i < cap; i++) out[i] = mpj_int(a->items[i]);
    *n = a->n;
}

static const mpj *path(const mpj *root, const char *a, const char *b, const char *c)
{
    const mpj *v = mpj_get(root, a);
    if (b) v = mpj_get(v, b);
    if (c) v = mpj_get(v, c);
    return v;
}

/* Row of a uid-keyed multimap. */
static int row(const mpj *mm, long long uid, long long *out, int cap)
{
    long long kv[256], vals[256];
    int nk, nv, i, n = 0;
    ints_of(mpj_get(mm, "keyValues"), kv, 256, &nk);
    ints_of(mpj_get(mm, "values"), vals, 256, &nv);
    if (uid + 1 >= nk) return 0;
    for (i = (int)kv[uid]; i < kv[uid + 1] && n < cap; i++) out[n++] = vals[i];
    return n;
}

static void test_json_roundtrip(void)
{
    const char *t = "{\"a\":[1,-2.50,3e-05,\"x\\\"y\"],\"b\":{\"c\":true,\"d\":null},\"e\":1280.0}";
    mpj *v = mpj_parse(t, strlen(t));
    size_t n = 0;
    char *d;
    CHECK(v != NULL);
    d = mpj_dump(v, &n);
    CHECK(d && !strcmp(d, t));
    free(d);
    mpj_free(v);
    CHECK(mpj_parse("{\"a\":1,}", 8) == NULL);
    CHECK(mpj_parse("[1 2]", 5) == NULL);
    CHECK(mpj_parse("{\"a\":1} x", 9) == NULL);
}

static void test_is_module_file(void)
{
    const char *a = "{\"customFilterAllocCount\":0,\"x\":{\"~type\":\"idSnapEntityPrefab\"},\"~type\":\"idSnapMap\"}";
    const char *b = "{\"entities\":[{\"~type\":\"idSnapMap\"}],\"~type\":\"idSnapEntityPrefab\"}";
    const char *c = "  {\"~type\" : \"idSnapMap\"}";
    CHECK(sh_mp_is_module_file(a, strlen(a)) == 1);
    CHECK(sh_mp_is_module_file(b, strlen(b)) == 0);
    CHECK(sh_mp_is_module_file(c, strlen(c)) == 1);
    CHECK(sh_mp_is_module_file("[]", 2) == 0);
}

static void test_cut_and_merge(void)
{
    char *src = source_map(), *frag = NULL, *merged = NULL, *tgt = NULL, err[256] = "";
    size_t flen = 0, mlen = 0;
    int sel[2] = { 1, 0 }, first = -1, added = 0;
    sh_mp_summary sum;
    mpj *f, *m;
    long long v[64], r[8];
    int n;
    float at[3] = { 256.0f, 512.0f, 0.0f };

    CHECK(src != NULL);
    CHECK(sh_mp_cut(src, strlen(src), sel, 2, &frag, &flen, &sum, err, sizeof err) == 1);
    if (!frag) { printf("cut failed: %s\n", err); g_fail = 1; free(src); return; }
    CHECK(sum.module_count == 2);
    CHECK(sum.entity_count == 7);          /* 56 57 58 59 60 61 70 */
    CHECK(sum.door_count == 4);            /* b's two slots, then a's two */
    CHECK(sum.has_nav_boxes == 1);
    CHECK(sum.dropped_wires == 2);         /* 60->63 and 64->60 */
    CHECK(sum.dropped_refs == 0);          /* 61 -> 60 is inside */
    CHECK(sum.variable_count == 1);        /* number 1 */
    CHECK(sum.env_count == 1 && !strcmp(sum.envs[0], "snapmap/base"));
    CHECK(!strcmp(sum.modules[0].name, "maps/modules/b.decl") && sum.modules[0].rotation == 2);
    CHECK(sum.modules[0].origin[0] == 0 && sum.modules[0].origin[1] == 0);
    CHECK(sum.modules[1].origin[0] == 0 && sum.modules[1].origin[1] == -2048.0f);

    f = mpj_parse(frag, flen);
    CHECK(f != NULL);
    ints_of(path(f, "instanceEntities", "keyValues", NULL), v, 64, &n);
    CHECK(n == 4 && v[0] == 0 && v[1] == 3 && v[2] == 7 && v[3] == 7);
    ints_of(path(f, "instanceEntities", "values", NULL), v, 64, &n);
    CHECK(n == 7 && v[0] == 58 && v[1] == 59 && v[2] == 61 && v[3] == 56 && v[6] == 70);
    ints_of(path(f, "doorsAndCaps", "portalDoors", NULL), v, 64, &n);
    CHECK(n == 4 && v[0] == 58 && v[1] == 59 && v[2] == 56 && v[3] == 57);
    ints_of(path(f, "doorsAndCaps", "portalFrames", NULL), v, 64, &n);
    CHECK(n == 4 && v[0] == -1 && v[3] == 70);
    ints_of(path(f, "targets", "connections", NULL), v, 64, &n);
    CHECK(n == 4 && v[0] == -61 && v[1] == 61 && v[2] == -62 && v[3] == 60);
    CHECK(path(f, "variables", "number", NULL)->n == 1);
    CHECK(mpj_str_is(mpj_get(mpj_get(path(f, "variables", "number", NULL)->items[0], "info"), "name"), "Number 2"));
    CHECK(path(f, "variables", "string", NULL)->n == 0);      /* smp.* not carried */
    CHECK(path(f, "variables", "persistentInteger", NULL)->n == 1);
    ints_of(path(f, "variables", "allocCount", NULL), v, 64, &n);
    CHECK(n == 10 && v[1] == 1 && v[4] == 0);
    n = row(path(f, "references", "entityVarRefs", NULL), 60, r, 8);
    CHECK(n == 1 && r[0] == ((1 << 24) | 0));                  /* number 1 -> index 0 */
    n = row(path(f, "references", "entityEntRefs", NULL), 61, r, 8);
    CHECK(n == 1 && r[0] == 60);
    n = row(path(f, "references", "entityEntRefs", NULL), 63, r, 8);
    CHECK(n == 0);                                             /* 63 not in the set */
    CHECK(path(f, "entities", NULL, NULL)->n == 7);
    mpj_free(f);

    /* re-derive */
    {
        sh_mp_summary s2;
        CHECK(sh_mp_inspect(frag, flen, &s2, err, sizeof err) == 1);
        CHECK(s2.module_count == 2 && s2.entity_count == 7 && s2.door_count == 4 && s2.has_nav_boxes == 1);
        sh_mp_summary_free(&s2);
    }

    /* sidecar */
    {
        float b[2][6] = { { -1024, -1024, 0, 1024, 1024, 256 }, { -1024, -3072, 0, 1024, -1024, 256 } };
        const char *labels[2] = { "Room B", NULL };
        char *sc = sh_mp_sidecar_json(&sum, (const float (*)[6])b, labels);
        mpj *j = sc ? mpj_parse(sc, strlen(sc)) : NULL;
        CHECK(j != NULL);
        CHECK(mpj_str_is(mpj_get(j, "kind"), "modules"));
        CHECK(mpj_int(mpj_get(j, "moduleCount")) == 2);
        CHECK(mpj_get(j, "hasNavigationBoxes") && mpj_get(j, "hasNavigationBoxes")->b == 1);
        CHECK(mpj_get(mpj_get(j, "modules"), "x") == NULL);
        CHECK(mpj_str_is(mpj_get(mpj_get(j, "modules")->items[0], "label"), "Room B"));
        ints_of(mpj_get(mpj_get(j, "bounds"), "min"), v, 8, &n);
        CHECK(n == 3 && v[0] == -1024 && v[1] == -3072);
        ints_of(mpj_get(mpj_get(j, "modules")->items[1], "size"), v, 8, &n);
        CHECK(n == 3 && v[0] == 2048 && v[1] == 2048 && v[2] == 256);
        CHECK(mpj_get(j, "desc") && mpj_get(j, "tags"));
        mpj_free(j);
        free(sc);
    }

    /* merge into the target */
    tgt = target_map();
    CHECK(tgt != NULL);
    CHECK(sh_mp_merge(tgt, strlen(tgt), frag, flen, at, &merged, &mlen, &first, &added, err, sizeof err) == 1);
    if (!merged) { printf("merge failed: %s\n", err); g_fail = 1; goto out; }
    CHECK(first == 1 && added == 2);
    m = mpj_parse(merged, mlen);
    CHECK(m != NULL);
    CHECK(path(m, "instances", NULL, NULL)->n == 3);
    {
        float o[3];
        const mpj *org = mpj_get(path(m, "instances", NULL, NULL)->items[2], "origin");
        o[0] = (float)mpj_num(mpj_get(org, "x")); o[1] = (float)mpj_num(mpj_get(org, "y"));
        CHECK(o[0] == 256.0f && o[1] == -1536.0f);
    }
    /* ids: target max 61, stale row at 62 skipped -> 63.. */
    ints_of(path(m, "instanceEntities", "keyValues", NULL), v, 64, &n);
    CHECK(n == 5 && v[0] == 0 && v[1] == 3 && v[2] == 6 && v[3] == 10 && v[4] == 10);
    ints_of(path(m, "instanceEntities", "values", NULL), v, 64, &n);
    /* target group 0: 56,57,61; then the prefab groups, ids in prefab entity order:
     * 56->63, 57->64, 58->65, 59->66, 60->67, 61->68, 70->69 */
    CHECK(n == 10 && v[0] == 56 && v[1] == 57 && v[2] == 61 && v[3] == 65 && v[4] == 66 &&
          v[5] == 68 && v[6] == 63 && v[7] == 64 && v[8] == 67 && v[9] == 69);
    ints_of(path(m, "doorsAndCaps", "portalDoors", NULL), v, 64, &n);
    CHECK(n == 6 && v[2] == 65 && v[3] == 66 && v[4] == 63 && v[5] == 64);
    ints_of(path(m, "doorsAndCaps", "portalFrames", NULL), v, 64, &n);
    CHECK(n == 6 && v[5] == 69 && v[2] == -1);
    ints_of(path(m, "targets", "connections", NULL), v, 64, &n);
    CHECK(n == 4 && v[0] == -68 && v[1] == 68 && v[2] == -69 && v[3] == 67);
    n = row(path(m, "references", "entityEntRefs", NULL), 68, r, 8);
    CHECK(n == 1 && r[0] == 67);
    n = row(path(m, "references", "entityEntRefs", NULL), 62, r, 8);
    CHECK(n == 1 && r[0] == -1);                               /* stale row kept */
    n = row(path(m, "references", "entityEntRefs", NULL), 61, r, 8);
    CHECK(n == 1);
    n = row(path(m, "references", "entityVarRefs", NULL), 67, r, 8);
    CHECK(n == 1 && r[0] == ((1 << 24) | 0));                  /* target had no numbers */
    n = row(path(m, "references", "entityEntRefs", NULL), 69, r, 8);
    CHECK(n == 0);                                             /* the frame owns no slot */
    ints_of(path(m, "references", "entityEntRefs", "keyValues"), v, 64, &n);
    CHECK(n == 71);                                            /* rows for ids 0..69, max id + 2 keys */
    CHECK(path(m, "variables", "number", NULL)->n == 1);
    CHECK(path(m, "variables", "boolean", NULL)->n == 2);
    ints_of(path(m, "variables", "allocCount", NULL), v, 64, &n);
    CHECK(n == 10 && v[1] == 1 && v[2] == 3);
    CHECK(path(m, "entities", NULL, NULL)->n == 10);
    {
        const mpj *e = path(m, "entities", NULL, NULL)->items[3];
        CHECK(mpj_int(mpj_get(e, "uniqueId")) == 63);
    }
    mpj_free(m);

    /* merging again mints ids above the first copy */
    {
        char *twice = NULL;
        size_t tlen = 0;
        CHECK(sh_mp_merge(merged, mlen, frag, flen, at, &twice, &tlen, &first, &added, err, sizeof err) == 1);
        m = twice ? mpj_parse(twice, tlen) : NULL;
        CHECK(m && path(m, "instances", NULL, NULL)->n == 5 && first == 3);
        CHECK(m && mpj_int(mpj_get(path(m, "entities", NULL, NULL)->items[10], "uniqueId")) == 70);
        mpj_free(m);
        free(twice);
    }
out:
    sh_mp_summary_free(&sum);
    free(frag); free(merged); free(src); free(tgt);
}

static void test_refusals(void)
{
    char *src = source_map(), *frag = NULL, err[256] = "";
    size_t flen = 0;
    sh_mp_summary sum;
    int bad[1] = { 7 }, dup[2] = { 1, 1 };
    float at[3] = { 0, 0, 0 };
    char *out = NULL;
    CHECK(sh_mp_cut(src, strlen(src), bad, 1, &frag, &flen, &sum, err, sizeof err) == 0 && frag == NULL);
    CHECK(sh_mp_cut(src, strlen(src), dup, 2, &frag, &flen, &sum, err, sizeof err) == 0 && frag == NULL);
    CHECK(sh_mp_cut("{\"~type\":\"idSnapEntityPrefab\"}", 30, dup, 1, &frag, &flen, &sum, err, sizeof err) == 0);
    char *tgt = target_map();
    CHECK(sh_mp_merge(tgt, strlen(tgt), "{\"~type\":\"idSnapEntityPrefab\"}", 30, at, &out, &flen,
                      NULL, NULL, err, sizeof err) == 0 && out == NULL);
    free(src); free(tgt);
}

int main(void)
{
    test_json_roundtrip();
    test_is_module_file();
    test_cut_and_merge();
    test_refusals();
    if (g_fail) { printf("module_prefab_test: FAILED\n"); return 1; }
    printf("module_prefab_test: all passed\n");
    return 0;
}
