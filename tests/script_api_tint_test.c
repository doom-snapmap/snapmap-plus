/* script_api_tint_test.c -- the pure half of e:tint and e:glow (src/backend/script/script_api_tint.h): the
 * render parms a look is written as, the channel and seconds checks, the fade length, the look a fade shows at a
 * time, a request made in the middle of a fade, the half of the look a request leaves alone, and the table's
 * round-robin replacement. Nothing here touches the engine. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "script/script_api_tint.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

static int near3(const float *a, float x, float y, float z)
{
    return fabsf(a[0] - x) < 1e-5f && fabsf(a[1] - y) < 1e-5f && fabsf(a[2] - z) < 1e-5f;
}

static sh_tint_table g_table;

static void test_parms(void)
{
    sh_tint_state s;
    sh_tint_parm p[SH_TINT_PARMS];
    sh_tint_neutral(&s);
    CHECK(sh_tint_is_neutral(&s));
    sh_tint_parms(&s, p);
    /* the neutral look is each parm's engine default: color {1,1,1,1}, bloomcolor {1,1,1,1}, highlightcolor {0,0,0,0} */
    CHECK(!strcmp(p[0].name, "color") && near3(p[0].v, 1, 1, 1) && p[0].v[3] == 1.0f);
    CHECK(!strcmp(p[1].name, "bloomcolor") && near3(p[1].v, 1, 1, 1) && p[1].v[3] == 1.0f);
    CHECK(!strcmp(p[2].name, "highlightcolor") && near3(p[2].v, 0, 0, 0) && p[2].v[3] == 0.0f);
    /* a tint multiplies color and washes by tint - 1; a glow adds to bloomcolor and to the wash */
    s.tint[0] = 2.0f; s.tint[1] = 0.5f; s.tint[2] = 1.0f;
    sh_tint_parms(&s, p);
    CHECK(!sh_tint_is_neutral(&s));
    CHECK(near3(p[0].v, 2, 0.5f, 1) && near3(p[1].v, 1, 1, 1) && near3(p[2].v, 1, -0.5f, 0));
    s.glow[0] = 0.25f; s.glow[1] = 3.0f; s.glow[2] = 0.0f;
    sh_tint_parms(&s, p);
    CHECK(near3(p[0].v, 2, 0.5f, 1) && near3(p[1].v, 1.25f, 4, 1) && near3(p[2].v, 1.25f, 2.5f, 0));
    /* the wash never subtracts more than the colour it is added to */
    sh_tint_neutral(&s);
    s.tint[0] = 0.0f;
    sh_tint_parms(&s, p);
    CHECK(near3(p[2].v, -1, 0, 0));
}

static void test_arguments(void)
{
    char err[200];
    CHECK(sh_tint_check_channel("tint", "the red", 0.0, SH_TINT_MAX_TINT, err, sizeof err));
    CHECK(sh_tint_check_channel("tint", "the red", 16.0, SH_TINT_MAX_TINT, err, sizeof err));
    CHECK(!sh_tint_check_channel("tint", "the red", -0.5, SH_TINT_MAX_TINT, err, sizeof err));
    CHECK(!strcmp(err, "\"tint\" needs the red from 0 to 16, got -0.5"));
    CHECK(!sh_tint_check_channel("glow", "the blue", 65.0, SH_TINT_MAX_GLOW, err, sizeof err));
    CHECK(!strcmp(err, "\"glow\" needs the blue from 0 to 64, got 65"));
    CHECK(!sh_tint_check_channel("glow", "the green", INFINITY, SH_TINT_MAX_GLOW, err, sizeof err));
    CHECK(!sh_tint_check_channel("glow", "the green", NAN, SH_TINT_MAX_GLOW, err, sizeof err));
    CHECK(strstr(err, "not a number") != NULL);
    CHECK(sh_tint_check_seconds("tint", 0.0, err, sizeof err));
    CHECK(sh_tint_check_seconds("tint", 3600.0, err, sizeof err));
    CHECK(!sh_tint_check_seconds("tint", -1.0, err, sizeof err));
    CHECK(!strcmp(err, "\"tint\" needs the seconds to fade over from 0 to 3600, got -1"));
    CHECK(!sh_tint_check_seconds("glow", 3601.0, err, sizeof err));
    CHECK(!sh_tint_check_seconds("glow", NAN, err, sizeof err));
    /* the engine's fade is whole milliseconds, never 0 */
    CHECK(sh_tint_duration_ms(0.0) == 1);
    CHECK(sh_tint_duration_ms(0.0004) == 1);
    CHECK(sh_tint_duration_ms(0.0016) == 2);
    CHECK(sh_tint_duration_ms(1.5) == 1500);
    CHECK(sh_tint_duration_ms(3600.0) == 3600000);
    CHECK(sh_tint_duration_ms(1e9) == 3600000);
}

static void test_fade(void)
{
    sh_tint_state from, to, now;
    sh_tint_rec *r;
    const float red[3] = { 1, 0, 0 }, tint[3] = { 0.5f, 0.5f, 0.5f }, none[3] = { 0, 0, 0 }, white[3] = { 1, 1, 1 };
    sh_tint_reset(&g_table);
    CHECK(sh_tint_find(&g_table, 7) == NULL && sh_tint_find(&g_table, 0) == NULL);
    /* a first request starts from the neutral look and goes to the one given, the other half neutral */
    r = sh_tint_begin(&g_table, 7, 1, red, 1000, 2000, &from, &to);
    CHECK(r && r->handle == 7 && sh_tint_find(&g_table, 7) == r);
    CHECK(sh_tint_is_neutral(&from));
    CHECK(near3(to.glow, 1, 0, 0) && near3(to.tint, 1, 1, 1));
    /* the fade: nothing before it starts, linear while it runs, the target after */
    sh_tint_at(r, 500, &now);
    CHECK(near3(now.glow, 0, 0, 0));
    sh_tint_at(r, 1000, &now);
    CHECK(near3(now.glow, 0, 0, 0));
    sh_tint_at(r, 2000, &now);
    CHECK(near3(now.glow, 0.5f, 0, 0));
    sh_tint_at(r, 3000, &now);
    CHECK(near3(now.glow, 1, 0, 0));
    sh_tint_at(r, 99999, &now);
    CHECK(near3(now.glow, 1, 0, 0) && near3(now.tint, 1, 1, 1));
    /* a request in the middle of the fade starts where the fade is; the tint is the half it leaves alone, and a
     * tint asked for now keeps the glow's target */
    r = sh_tint_begin(&g_table, 7, 0, tint, 2000, 1000, &from, &to);
    CHECK(near3(from.glow, 0.5f, 0, 0) && near3(from.tint, 1, 1, 1));
    CHECK(near3(to.glow, 1, 0, 0) && near3(to.tint, 0.5f, 0.5f, 0.5f));
    sh_tint_at(r, 2500, &now);
    CHECK(near3(now.tint, 0.75f, 0.75f, 0.75f) && near3(now.glow, 0.75f, 0, 0));
    /* a request on the same instant as the last starts from that request's own start look */
    r = sh_tint_begin(&g_table, 7, 1, none, 2000, 1, &from, &to);
    CHECK(near3(from.glow, 0.5f, 0, 0) && near3(from.tint, 1, 1, 1));
    CHECK(near3(to.glow, 0, 0, 0) && near3(to.tint, 0.5f, 0.5f, 0.5f));
    /* a finished fade is the look to go on from; neutral at both ends is neutral */
    r = sh_tint_begin(&g_table, 7, 0, white, 9000, 1, &from, &to);
    CHECK(near3(from.tint, 0.5f, 0.5f, 0.5f) && near3(to.tint, 1, 1, 1) && near3(to.glow, 0, 0, 0));
    sh_tint_at(r, 9001, &now);
    CHECK(sh_tint_is_neutral(&now));
    /* a zero-length record is its target */
    r->dur_ms = 0;
    sh_tint_at(r, 0, &now);
    CHECK(sh_tint_is_neutral(&now));
}

static void test_table(void)
{
    uint32_t h;
    sh_tint_state from, to;
    const float c[3] = { 2, 2, 2 };
    sh_tint_rec *r;
    sh_tint_reset(&g_table);
    for (h = 1; h <= SH_TINT_RECORDS; h++) sh_tint_begin(&g_table, h, 0, c, 0, 1, &from, &to);
    CHECK(g_table.next == 0);
    /* the same entity again reuses its record and displaces nobody */
    sh_tint_begin(&g_table, 5, 0, c, 10, 1, &from, &to);
    CHECK(g_table.next == 0 && sh_tint_find(&g_table, 1) != NULL);
    CHECK(near3(from.tint, 2, 2, 2));
    /* a new entity takes the oldest record; the displaced one starts from the neutral look */
    r = sh_tint_begin(&g_table, 1000, 0, c, 20, 1, &from, &to);
    CHECK(r && r->handle == 1000 && g_table.next == 1);
    CHECK(sh_tint_find(&g_table, 1) == NULL && sh_tint_find(&g_table, 2) != NULL);
    sh_tint_begin(&g_table, 1, 0, c, 30, 1, &from, &to);
    CHECK(sh_tint_is_neutral(&from));
}

/* glow's strength: the colour times the strength; no strength is the soft default, an explicit 1 is the full
 * glow e:glow gave before the strength (its wash the whole colour), and the product stays within the glow's range. */
static void test_glow_strength(void)
{
    char err[200];
    float rgb[3] = { 0.0f, 0.6f, 1.0f };
    sh_tint_state s;
    sh_tint_parm p[SH_TINT_PARMS];
    CHECK(SH_TINT_GLOW_STRENGTH_DEFAULT > 0.0 && SH_TINT_GLOW_STRENGTH_DEFAULT < 1.0);
    CHECK(sh_tint_glow_scaled("glow", rgb, SH_TINT_GLOW_STRENGTH_DEFAULT, err, sizeof err));
    CHECK(near3(rgb, 0.0f, (float)(0.6 * SH_TINT_GLOW_STRENGTH_DEFAULT), (float)SH_TINT_GLOW_STRENGTH_DEFAULT));
    sh_tint_neutral(&s);
    memcpy(s.glow, rgb, sizeof rgb);
    sh_tint_parms(&s, p);
    /* the soft default washes the surface with only part of the colour */
    CHECK(p[2].v[2] > 0.0f && p[2].v[2] < 1.0f && near3(p[1].v, 1.0f, 1.0f + rgb[1], 1.0f + rgb[2]));
    rgb[0] = 0.0f; rgb[1] = 0.6f; rgb[2] = 1.0f;
    CHECK(sh_tint_glow_scaled("glow", rgb, 1.0, err, sizeof err) && near3(rgb, 0.0f, 0.6f, 1.0f));
    rgb[0] = 0.0f; rgb[1] = 0.6f; rgb[2] = 1.0f;
    CHECK(sh_tint_glow_scaled("glow", rgb, 0.0, err, sizeof err) && near3(rgb, 0.0f, 0.0f, 0.0f));
    rgb[0] = 8.0f; rgb[1] = 0.0f; rgb[2] = 0.0f;
    CHECK(!sh_tint_glow_scaled("glow", rgb, 16.0, err, sizeof err) && rgb[0] == 8.0f);
    CHECK(!strcmp(err, "\"glow\" glows at most 64 in a channel, got 128 (colour 8 at strength 16)"));
    CHECK(sh_tint_check_strength("glow", 0.0, err, sizeof err) && sh_tint_check_strength("glow", 16.0, err, sizeof err));
    CHECK(!sh_tint_check_strength("glow", -0.1, err, sizeof err));
    CHECK(!strcmp(err, "\"glow\" needs the strength from 0 to 16, got -0.1"));
    CHECK(!sh_tint_check_strength("glow", NAN, err, sizeof err));
}

int main(void)
{
    test_parms();
    test_arguments();
    test_glow_strength();
    test_fade();
    test_table();
    printf(g_fail ? "script_api_tint_test: FAILED\n" : "script_api_tint_test: ok\n");
    return g_fail;
}
