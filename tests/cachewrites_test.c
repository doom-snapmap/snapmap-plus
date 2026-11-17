/* cachewrites_test.c -- which of the engine's writes is a cache write, and where it goes (cachewrites_rule.c). */
#include <stdio.h>
#include <string.h>
#include "cachewrites.h"

static int g_checks, g_failed;
#define CHECK(x) do { g_checks++; if (!(x)) { g_failed++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)

#define ROOT "C:\\Users\\p\\Saved Games\\id Software\\DOOM"

/* The rule's answer for rel under ROOT, and that a refusal leaves out untouched. */
static int goes_to(const char *rel, const char *want)
{
    char out[512];
    memset(out, 'x', sizeof out);
    out[sizeof out - 1] = '\0';
    if (!sh_cachewrite_redirect(rel, ROOT, out, sizeof out)) return want == NULL && out[0] == 'x' && out[1] == 'x';
    return want != NULL && strcmp(out, want) == 0;
}

int main(void)
{
    char out[128];
    size_t need;
    printf("cachewrites_test\n");

    printf("-- the engine's cache writes\n");
    CHECK(goes_to("generated/cm/m1.bcm", ROOT "\\base\\generated\\cm\\m1.bcm"));
    CHECK(goes_to("generated/models/effects/particles/a.pmodel",
                  ROOT "\\base\\generated\\models\\effects\\particles\\a.pmodel"));
    CHECK(goes_to("generated/temp/maps/x.bcm.1A2B.tmp", ROOT "\\base\\generated\\temp\\maps\\x.bcm.1A2B.tmp"));
    CHECK(goes_to("maps/DynamicSnapMap.flight", ROOT "\\base\\maps\\DynamicSnapMap.flight"));
    CHECK(goes_to("maps/x.flight", ROOT "\\base\\maps\\x.flight"));
    CHECK(goes_to("generated/", ROOT "\\base\\generated\\"));

    printf("-- either slash, any case\n");
    CHECK(goes_to("generated\\cm\\m1.bcm", ROOT "\\base\\generated\\cm\\m1.bcm"));
    CHECK(goes_to("generated\\cm/m1.bcm", ROOT "\\base\\generated\\cm\\m1.bcm"));
    CHECK(goes_to("GENERATED/CM/M1.BCM", ROOT "\\base\\GENERATED\\CM\\M1.BCM"));
    CHECK(goes_to("Maps\\Sub\\X.FLIGHT", ROOT "\\base\\Maps\\Sub\\X.FLIGHT"));

    printf("-- every other write passes\n");
    CHECK(goes_to("generated", NULL));
    CHECK(goes_to("generatedx/a.bcm", NULL));
    CHECK(goes_to("maps/x.map", NULL));
    CHECK(goes_to("maps/x.flight.bak", NULL));
    CHECK(goes_to("mapsx/x.flight", NULL));
    CHECK(goes_to("x.flight", NULL));
    CHECK(goes_to("savegame.user/a", NULL));
    CHECK(goes_to("base/generated/cm/a.bcm", NULL));

    printf("-- absolute and parent paths are refused\n");
    CHECK(goes_to("C:/generated/a.bcm", NULL));
    CHECK(goes_to("d:\\generated\\a.bcm", NULL));
    CHECK(goes_to("/generated/a.bcm", NULL));
    CHECK(goes_to("\\generated\\a.bcm", NULL));
    CHECK(goes_to("generated/../a.bcm", NULL));
    CHECK(goes_to("generated\\..\\..\\a.bcm", NULL));
    CHECK(goes_to("maps/../x.flight", NULL));
    CHECK(goes_to("generated/..", NULL));
    CHECK(goes_to("generated/..a/b.bcm", ROOT "\\base\\generated\\..a\\b.bcm"));
    CHECK(goes_to("generated/a../b.bcm", ROOT "\\base\\generated\\a..\\b.bcm"));

    printf("-- NULL and empty\n");
    CHECK(!sh_cachewrite_redirect(NULL, ROOT, out, sizeof out));
    CHECK(!sh_cachewrite_redirect("", ROOT, out, sizeof out));
    CHECK(!sh_cachewrite_redirect("generated/a", NULL, out, sizeof out));
    CHECK(!sh_cachewrite_redirect("generated/a", "", out, sizeof out));
    CHECK(!sh_cachewrite_redirect("generated/a", ROOT, NULL, sizeof out));
    CHECK(!sh_cachewrite_redirect("generated/a", ROOT, out, 0));

    printf("-- a trailing slash on the root is not doubled\n");
    CHECK(sh_cachewrite_redirect("generated/a", "C:\\r\\", out, sizeof out) && !strcmp(out, "C:\\r\\base\\generated\\a"));
    CHECK(sh_cachewrite_redirect("generated/a", "C:\\r/", out, sizeof out) && !strcmp(out, "C:\\r\\base\\generated\\a"));
    CHECK(sh_cachewrite_redirect("generated/a", "C:\\r", out, sizeof out) && !strcmp(out, "C:\\r\\base\\generated\\a"));

    printf("-- the result must fit with its terminator\n");
    need = strlen("C:\\r\\base\\generated\\a") + 1;
    memset(out, 'x', sizeof out);
    CHECK(!sh_cachewrite_redirect("generated/a", "C:\\r", out, need - 1));
    CHECK(out[0] == 'x');
    CHECK(sh_cachewrite_redirect("generated/a", "C:\\r", out, need) && strlen(out) == need - 1);

    printf("cachewrites_test: %d checks, %d failed\n", g_checks, g_failed);
    printf(g_failed ? "cachewrites_test: FAIL\n" : "cachewrites_test: PASS\n");
    return g_failed ? 1 : 0;
}
