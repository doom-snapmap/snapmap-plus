/* map_carrier_test -- the map's "snapmap-plus" root member (map_carrier.h):
 * found only at the root, read by part, stripped, written compact and last
 * with its parts in order, removed when it carries nothing, refused (read as
 * absent, still stripped) when malformed, and a map without it untouched;
 * the map's place (a plain string, after its mods); the earlier-shape
 * detector (sh_map_old_shape). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/backend/map_carrier.h"

static int g_failed, g_checks;
#define CHECK(expr) do {                                                         \
    g_checks++;                                                                  \
    if (!(expr)) {                                                               \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++;                                                              \
    }                                                                            \
} while (0)

/* An engine map, compact, with an author variable that mentions the key. */
#define ENGINE_HEAD "{\"entities\":[],\"variables\":{\"allocCount\":[0,0,0,0,1,0,0,0,0,0],\"string\":[" \
    "{\"info\":{\"name\":\"snapmap-plus\",\"~type\":\"snapVarInfo_t\"},\"initialValue\":\"\\\"snapmap-plus\\\":1\"," \
    "\"~type\":\"snapVarString_t\"}],\"~type\":\"idSnapVariables\"},\"version\":111,\"~type\":\"idSnapMap\""
static const char ENGINE[] = ENGINE_HEAD "}";

#define MODS "[{\"mod\":\"aaaabbbb@x\",\"size\":1,\"names\":[]}]"
#define SETTINGS "{\"render\":\"4;0\",\"bounds\":\"1;32671\"}"
#define NAV "[{\"module\":\"a/b\",\"class\":\"monster48\",\"digest\":\"0123456789abcdef\",\"geom\":\"fedcba9876543210\",\"data\":[\"QUJD\",\"RUZH\"]}]"
#define MEMBER(parts) ",\"snapmap-plus\":{\"format\":1" parts "}"

static char *put(const char *json, const char *part, const char *value, size_t *n, char *err, size_t cap)
{
    return sh_carrier_put(json, strlen(json), part, value, value ? strlen(value) : 0, n, err, cap);
}

static int span_is(const char *json, size_t from, size_t to, const char *want)
{
    return to - from == strlen(want) && !memcmp(json + from, want, to - from);
}

static void vanilla(void)
{
    sh_carrier_view v;
    size_t n = 7;
    char err[128];
    printf("-- a map without the member: untouched, and the author's own text is not a member\n");
    CHECK(sh_carrier_view_read(ENGINE, sizeof ENGINE - 1, &v) == 1 && v.state == SH_CARRIER_ABSENT && v.members == 0);
    CHECK(sh_carrier_strip(ENGINE, sizeof ENGINE - 1, &n) == NULL && n == 0);
    CHECK(sh_carrier_part(ENGINE, sizeof ENGINE - 1, "mods", NULL, NULL) == 0);
    /* Removing a part the map does not carry changes nothing. */
    CHECK(put(ENGINE, "mods", NULL, &n, err, sizeof err) == NULL && !err[0]);
    CHECK(put(ENGINE, "nav", NULL, &n, err, sizeof err) == NULL && !err[0]);
    /* A member nested below the root is not the carrier. */
    {
        static const char nested[] = "{\"a\":{\"snapmap-plus\":{\"format\":1,\"mods\":[]}}}";
        CHECK(sh_carrier_view_read(nested, sizeof nested - 1, &v) == 1 && v.state == SH_CARRIER_ABSENT);
        CHECK(sh_carrier_strip(nested, sizeof nested - 1, &n) == NULL);
    }
    /* Text that is not one object does not read. */
    CHECK(sh_carrier_view_read("[1]", 3, &v) == 0);
    CHECK(sh_carrier_view_read("{\"snapmap-plus\":{\"format\":1}", 28, &v) == 0);
    CHECK(put("not json", "mods", "[]", &n, err, sizeof err) == NULL && err[0]);
    CHECK(put(ENGINE, "other", "[]", &n, err, sizeof err) == NULL && err[0]);
}

static void three_parts(void)
{
    char err[128], *a, *b, *c, *d;
    size_t n = 0, from = 0, to = 0;
    sh_carrier_view v;
    static const char ALL[] = ENGINE_HEAD MEMBER(",\"mods\":" MODS ",\"settings\":" SETTINGS ",\"nav\":" NAV) "}";
    printf("-- all three parts: written in order whatever the order put, read back, removed one by one\n");
    /* The save's order is settings, nav, mods; the member's is mods, settings, nav. */
    a = put(ENGINE, "settings", SETTINGS, &n, err, sizeof err);
    CHECK(a && n == strlen(a) && !strcmp(a, ENGINE_HEAD MEMBER(",\"settings\":" SETTINGS) "}"));
    b = a ? put(a, "nav", NAV, &n, err, sizeof err) : NULL;
    c = b ? put(b, "mods", MODS, &n, err, sizeof err) : NULL;
    CHECK(c && !strcmp(c, ALL));
    if (c) {
        CHECK(sh_carrier_view_read(c, n, &v) == 1 && v.state == SH_CARRIER_PRESENT && v.members == 1);
        CHECK(v.mods.present && span_is(c, v.mods.from, v.mods.to, MODS));
        CHECK(v.settings.present && span_is(c, v.settings.from, v.settings.to, SETTINGS));
        CHECK(v.nav.present && span_is(c, v.nav.from, v.nav.to, NAV));
        CHECK(sh_carrier_part(c, n, "nav", &from, &to) == 1 && span_is(c, from, to, NAV));
        /* Putting a part to what it already is changes nothing. */
        CHECK(put(c, "mods", MODS, &n, err, sizeof err) == NULL && !err[0]);
        /* Strip gives the engine's bytes back. */
        d = sh_carrier_strip(c, strlen(c), &n);
        CHECK(d && n == sizeof ENGINE - 1 && !strcmp(d, ENGINE));
        if (d) HeapFree(GetProcessHeap(), 0, d);
        /* One part removed: the others stay, in order. */
        d = put(c, "settings", NULL, &n, err, sizeof err);
        CHECK(d && !strcmp(d, ENGINE_HEAD MEMBER(",\"mods\":" MODS ",\"nav\":" NAV) "}"));
        if (d) {
            char *e = put(d, "mods", NULL, &n, err, sizeof err), *f = e ? put(e, "nav", NULL, &n, err, sizeof err) : NULL;
            /* Nothing left to carry: the member goes and the bytes are the engine's. */
            CHECK(f && !strcmp(f, ENGINE));
            if (e) HeapFree(GetProcessHeap(), 0, e);
            if (f) HeapFree(GetProcessHeap(), 0, f);
            HeapFree(GetProcessHeap(), 0, d);
        }
    }
    if (a) HeapFree(GetProcessHeap(), 0, a);
    if (b) HeapFree(GetProcessHeap(), 0, b);
    if (c) HeapFree(GetProcessHeap(), 0, c);
}

static void placement(void)
{
    char err[128], *out;
    size_t n = 0;
    sh_carrier_view v;
    printf("-- found anywhere at the root, rewritten last; whitespace and the first-member case\n");
    /* A member not last (written by hand) is moved to the end. */
    {
        static const char first[] = "{\"snapmap-plus\":{\"format\":1,\"mods\":[]} , \"version\":111}";
        CHECK(sh_carrier_view_read(first, sizeof first - 1, &v) == 1 && v.state == SH_CARRIER_PRESENT);
        out = sh_carrier_strip(first, sizeof first - 1, &n);
        CHECK(out && !strcmp(out, "{\"version\":111}"));
        if (out) HeapFree(GetProcessHeap(), 0, out);
        out = put(first, "settings", "{}", &n, err, sizeof err);
        CHECK(out && !strcmp(out, "{\"version\":111,\"snapmap-plus\":{\"format\":1,\"mods\":[],\"settings\":{}}}"));
        if (out) HeapFree(GetProcessHeap(), 0, out);
    }
    {
        static const char middle[] = "{\"a\":1,\"snapmap-plus\":{\"format\":1},\"b\":2}";
        out = sh_carrier_strip(middle, sizeof middle - 1, &n);
        CHECK(out && !strcmp(out, "{\"a\":1,\"b\":2}"));
        if (out) HeapFree(GetProcessHeap(), 0, out);
    }
    /* The only member. */
    out = put("{}", "mods", "[]", &n, err, sizeof err);
    CHECK(out && !strcmp(out, "{\"snapmap-plus\":{\"format\":1,\"mods\":[]}}"));
    if (out) {
        char *back = sh_carrier_strip(out, n, &n);
        CHECK(back && !strcmp(back, "{}"));
        if (back) HeapFree(GetProcessHeap(), 0, back);
        HeapFree(GetProcessHeap(), 0, out);
    }
    /* Pretty text: read with its whitespace, written before the root's close. */
    {
        static const char pretty[] = "{\n  \"version\": 111,\n  \"snapmap-plus\" : { \"format\" : 1 ,\n"
                                     "    \"mods\" : [ ] }\n}\n";
        CHECK(sh_carrier_view_read(pretty, sizeof pretty - 1, &v) == 1 && v.state == SH_CARRIER_PRESENT &&
              v.mods.present && span_is(pretty, v.mods.from, v.mods.to, "[ ]"));
        out = sh_carrier_strip(pretty, sizeof pretty - 1, &n);
        CHECK(out && !strcmp(out, "{\n  \"version\": 111\n}\n"));
        if (out) HeapFree(GetProcessHeap(), 0, out);
    }
    /* Large values: a string of a megabyte is one token to skip. */
    {
        size_t big = 1u << 20, head;
        char *text = (char *)malloc(big + 128);
        CHECK(text != NULL);
        if (text) {
            head = (size_t)sprintf_s(text, 128, "{\"version\":111,\"snapmap-plus\":{\"format\":1,\"nav\":[\"");
            memset(text + head, 'A', big);
            strcpy_s(text + head + big, 128 - 1, "\"]}}");
            CHECK(sh_carrier_part(text, strlen(text), "nav", NULL, NULL) == 1);
            out = sh_carrier_strip(text, strlen(text), &n);
            CHECK(out && !strcmp(out, "{\"version\":111}"));
            if (out) HeapFree(GetProcessHeap(), 0, out);
            free(text);
        }
    }
}

static void malformed(void)
{
    sh_carrier_view v;
    size_t n = 0, i;
    char err[128], *out;
    static const char *const bad[] = {
        "{\"version\":111,\"snapmap-plus\":[1,2]}",                                   /* not an object */
        "{\"version\":111,\"snapmap-plus\":{\"mods\":[]}}",                            /* no format */
        "{\"version\":111,\"snapmap-plus\":{\"format\":2,\"mods\":[]}}",               /* another format */
        "{\"version\":111,\"snapmap-plus\":{\"format\":1,\"mods\":\"garbage\"}}",      /* a part of the wrong type */
        "{\"version\":111,\"snapmap-plus\":{\"format\":1,\"settings\":[]}}",
        "{\"version\":111,\"snapmap-plus\":{\"format\":1,\"nav\":{}}}",
        "{\"version\":111,\"snapmap-plus\":{\"format\":1,\"mods\":[],\"mods\":[]}}",   /* a part twice */
        "{\"version\":111,\"snapmap-plus\":{\"format\":1},\"snapmap-plus\":{\"format\":1}}",  /* the member twice */
    };
    printf("-- a malformed member is refused (read as absent) and still removed; the map is kept\n");
    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK(sh_carrier_view_read(bad[i], strlen(bad[i]), &v) == 1 && v.state == SH_CARRIER_MALFORMED && v.why[0]);
        CHECK(!v.mods.present && !v.settings.present && !v.nav.present);
        CHECK(sh_carrier_part(bad[i], strlen(bad[i]), "mods", NULL, NULL) == -1);
        out = sh_carrier_strip(bad[i], strlen(bad[i]), &n);
        CHECK(out && !strcmp(out, "{\"version\":111}"));
        if (out) HeapFree(GetProcessHeap(), 0, out);
        /* A save replaces it with a good one. */
        out = put(bad[i], "mods", "[]", &n, err, sizeof err);
        CHECK(out && !strcmp(out, "{\"version\":111,\"snapmap-plus\":{\"format\":1,\"mods\":[]}}"));
        if (out) HeapFree(GetProcessHeap(), 0, out);
        {
            char line[200];
            sh_carrier_describe(&v, line, sizeof line);
            CHECK(strstr(line, "refused") != NULL);
        }
    }
    /* Unknown parts are ignored on read and not written back. */
    {
        static const char future[] = "{\"snapmap-plus\":{\"format\":1,\"later\":{\"x\":[1]},\"mods\":[]}}";
        CHECK(sh_carrier_view_read(future, sizeof future - 1, &v) == 1 && v.state == SH_CARRIER_PRESENT && v.mods.present);
        out = put(future, "nav", "[]", &n, err, sizeof err);
        CHECK(out && !strcmp(out, "{\"snapmap-plus\":{\"format\":1,\"mods\":[],\"nav\":[]}}"));
        if (out) HeapFree(GetProcessHeap(), 0, out);
    }
}

static void readers(void)
{
    static const char obj[] = "{ \"render\" : \"4;0\", \"n\": 3, \"s\":\"a\\\"b\", \"data\": [\"x\" , \"y\"] }";
    const char *body = NULL, *cursor, *elem;
    size_t len = 0, count = 0;
    printf("-- object and array readers\n");
    CHECK(sh_carrier_object_string(obj, obj + sizeof obj - 1, "render", &body, &len) == 1 && len == 3 && !memcmp(body, "4;0", 3));
    CHECK(sh_carrier_object_string(obj, obj + sizeof obj - 1, "n", &body, &len) == -1);
    CHECK(sh_carrier_object_string(obj, obj + sizeof obj - 1, "s", &body, &len) == 1 && len == 4);
    CHECK(sh_carrier_object_string(obj, obj + sizeof obj - 1, "none", &body, &len) == 0);
    CHECK(sh_carrier_object_member(obj, obj + sizeof obj - 1, "data", &cursor, &len) == 1 && *cursor == '[');
    {
        const char *end = cursor + len;
        int r;
        while ((r = sh_carrier_array_next(&cursor, end, &elem, &len)) == 1) count++;
        CHECK(r == 0 && count == 2);
    }
    cursor = "[1,,2]";
    CHECK(sh_carrier_array_next(&cursor, cursor + 6, &elem, &len) == 1 &&
          sh_carrier_array_next(&cursor, cursor + 4, &elem, &len) == -1);
}

/* One pass names a map in an earlier shape: a variables.string name in a
 * retired family, or a member row with "id" or "bundle". A family's text that
 * is not a variable's name (prose, an entity name, a value) is not one. */
static void old_shapes(void)
{
    static const char *const families[] = {
        "smpkg.my_pack.0.1.0123456789abcdef", "smpbind.x.0123456789abcdef", "smpref.y.0123456789abcdef",
        "smpmod1.0.1", "smpprod1.v0.2.1", "smp.render.v1", "smp.bounds.v1",
        "smnav1.cat/mod.monster48.0.1.0123456789abcdef.fedcba9876543210"
    };
    char map[1024];
    size_t i;
    printf("-- earlier shapes: each family and an id row is named; the current shape passes\n");
    CHECK(sh_map_old_shape(ENGINE, sizeof ENGINE - 1) == SH_MAP_OLD_NONE);
    for (i = 0; i < sizeof families / sizeof families[0]; i++) {
        _snprintf_s(map, sizeof map, _TRUNCATE,
                    "{\"entities\":[],\"variables\":{\"allocCount\":[0,0,0,0,1],\"string\":[{\"info\":{\"name\" : \"%s\","
                    "\"~type\":\"snapVarInfo_t\"},\"initialValue\":\"QUJD\",\"~type\":\"snapVarString_t\"}]}}", families[i]);
        CHECK(sh_map_old_shape(map, strlen(map)) == SH_MAP_OLD_VARIABLES);
    }
    /* The same words elsewhere are not variables. */
    {
        static const char prose[] = "{\"entities\":[{\"note\":\"smpmod1.0.1\",\"inherit\":\"smp.render.v1\"}],"
                                    "\"variables\":{\"string\":[{\"info\":{\"name\":\"door\"},\"initialValue\":\"smnav1.x\"}]}}";
        CHECK(sh_map_old_shape(prose, sizeof prose - 1) == SH_MAP_OLD_NONE);
    }
    /* Member rows: the current row passes; an id or a bundle row is named. */
    {
        static const char now[] = ENGINE_HEAD MEMBER(",\"mods\":" MODS) "}";
        static const char id_row[] = ENGINE_HEAD MEMBER(",\"mods\":[{\"id\":\"a.b\",\"version\":\"1\",\"size\":1}]") "}";
        static const char bundle_row[] = ENGINE_HEAD MEMBER(",\"mods\":[" "{\"mod\":\"aaaabbbb@x\",\"size\":1},"
                                                            "{\"bundle\":\"x\",\"size\":1}]") "}";
        static const char names_only[] = ENGINE_HEAD MEMBER(",\"mods\":[{\"mod\":\"aaaabbbb@x\",\"names\":[\"id\",\"bundle\"]}]") "}";
        CHECK(sh_map_old_shape(now, sizeof now - 1) == SH_MAP_OLD_NONE);
        CHECK(sh_map_old_shape(id_row, sizeof id_row - 1) == SH_MAP_OLD_ROWS);
        CHECK(sh_map_old_shape(bundle_row, sizeof bundle_row - 1) == SH_MAP_OLD_ROWS);
        CHECK(sh_map_old_shape(names_only, sizeof names_only - 1) == SH_MAP_OLD_NONE);
    }
    CHECK(!strcmp(SH_MAP_OLD_REFUSAL, "This map was saved in a format this version does not read."));
}

static void place_part(void)
{
    char err[128], out[SH_CARRIER_PLACE_CAP], *a, *b, *c;
    size_t n = 0;
    printf("-- the place: a plain string after the mods, read back, removed with nothing else\n");
    a = put(ENGINE, "mods", MODS, &n, err, sizeof err);
    b = a ? sh_carrier_put_place(a, n, "Caf\xc3\xa9 arena", &n, err, sizeof err) : NULL;
    CHECK(b && !strcmp(b, ENGINE_HEAD MEMBER(",\"mods\":" MODS ",\"place\":\"Caf\xc3\xa9 arena\"") "}"));
    CHECK(b && sh_carrier_place(b, n, out, sizeof out) == 1 && !strcmp(out, "Caf\xc3\xa9 arena"));
    CHECK(sh_carrier_place(ENGINE, sizeof ENGINE - 1, out, sizeof out) == 0 && !out[0]);
    /* No place: removed; the mods stay. */
    c = b ? sh_carrier_put_place(b, n, NULL, &n, err, sizeof err) : NULL;
    CHECK(c && !strcmp(c, a));
    /* Not a folder name: refused, nothing written. */
    CHECK(sh_carrier_put_place(ENGINE, sizeof ENGINE - 1, "a\\b", &n, err, sizeof err) == NULL && err[0]);
    CHECK(sh_carrier_put_place(ENGINE, sizeof ENGINE - 1, "a\"b", &n, err, sizeof err) == NULL && err[0]);
    /* A place with an escape, or one too long, does not read; one of the
     * wrong type makes the member malformed. */
    {
        static const char esc[] = ENGINE_HEAD MEMBER(",\"place\":\"..\\\\x\"") "}";
        static const char num[] = ENGINE_HEAD MEMBER(",\"place\":7") "}";
        CHECK(sh_carrier_place(esc, sizeof esc - 1, out, sizeof out) == -1);
        CHECK(sh_carrier_place(num, sizeof num - 1, out, sizeof out) == -1);
        CHECK(sh_carrier_place(esc, sizeof esc - 1, out, 2) == -1);
    }
    if (a) HeapFree(GetProcessHeap(), 0, a);
    if (b) HeapFree(GetProcessHeap(), 0, b);
    if (c) HeapFree(GetProcessHeap(), 0, c);
}

int main(void)
{
    printf("map_carrier_test\n");
    vanilla();
    three_parts();
    placement();
    malformed();
    readers();
    place_part();
    old_shapes();
    printf("\n%s -- %d checks, %d failed\n", g_failed ? "FAILED" : "ok", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
