/* Bound nested scans and retain valid entity edits across scalar/list paths. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "json_patch.h"

static int failures;
#define CHECK(test) do { if (!(test)) { printf("FAIL line %d: %s\n", __LINE__, #test); failures++; } } while (0)

int main(void)
{
    char out[32768], nested[32768];
    const char *ids[] = {"one", "two"};
    CHECK(sh_json_patch_set_leaf("{}", "x.y", "1", out, sizeof out));
    CHECK(!strcmp(out, "{\"entityDef\":{\"state\":{\"edit\":{\"x\":{\"y\":1}}}}}"));
    CHECK(sh_json_patch_set_leaf("{\"keep\":[true,false,null,1.25e-2,\"\\u0041\"]}", "x", "\"ok\"", out, sizeof out));
    CHECK(sh_json_patch_upsert_reflist("{}", "targets", ids, 2, out, sizeof out));
    strcpy(nested, out);
    CHECK(sh_json_patch_upsert_reflist(nested, "targets", ids, 2, out, sizeof out));
    CHECK(!strcmp(nested, out));
    const char *bad[] = {"{\"x\":-}", "{\"x\":01}", "{\"x\":1.}", "{\"x\":1e+}",
        "{\"x\":[1,]}", "{\"x\":1,}", "{}junk", "{\"x\":\"\\q\"}", "{\"x\":\"\\u123\"}",
        "{\"x\":\"line\nbreak\"}", "{\"x\":trueish}"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK(!sh_json_patch_set_leaf(bad[i], "x", "1", out, sizeof out));
        CHECK(!sh_json_patch_upsert_reflist(bad[i], "targets", ids, 2, out, sizeof out));
    }
    CHECK(!sh_json_patch_set_leaf("{}", "x", "1 false", out, sizeof out));
    CHECK(!sh_json_patch_set_leaf("{}", "x..y", "1", out, sizeof out));
    CHECK(!sh_json_patch_set_leaf("{}", "x\"y", "1", out, sizeof out));
    CHECK(!sh_json_patch_upsert_reflist("{\"entityDef\":{\"state\":{\"edit\":{\"targets\":{\"num\":2147483648}}}}}",
                                      "targets", ids, 2, out, sizeof out));
    for (int objects = 0; objects < 2; objects++) {
        const int depths[] = {SH_JSON_PATCH_MAX_DEPTH, SH_JSON_PATCH_MAX_DEPTH + 1, 4000};
        for (int test = 0; test < 3; test++) {
            size_t n = 0;
            n += (size_t)sprintf(nested + n, "{\"keep\":");
            for (int i = 1; i < depths[test]; i++)
                n += (size_t)sprintf(nested + n, objects ? "{\"x\":" : "[");
            nested[n++] = '0';
            for (int i = 1; i < depths[test]; i++) nested[n++] = objects ? '}' : ']';
            nested[n++] = '}'; nested[n] = 0;
            CHECK(sh_json_patch_set_leaf(nested, "x", "1", out, sizeof out) == (test == 0));
            CHECK(sh_json_patch_upsert_reflist(nested, "targets", ids, 2, out, sizeof out) == (test == 0));
        }
    }
    CHECK(!sh_json_patch_set_leaf("{}", "x", "1", out, 4));
    CHECK(!sh_json_quote_string("x", NULL, 10));
    printf("json_patch_test: %d failures\n", failures);
    return failures != 0;
}
