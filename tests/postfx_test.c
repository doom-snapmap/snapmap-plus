/* postfx_test.c -- the radial blurs postfx.c keeps and the parameters it writes (postfx_blurs.c): which items are
 * drawn, the envelope, one blur per item however many views play it, the newest four drawn, the parameter layout
 * radialblur.frag reads. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "postfx.h"

static int g_checks, g_failed;
#define CHECK(x) do { g_checks++; if (!(x)) { g_failed++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)
#define NEAR(a, b) (fabsf((a) - (b)) < 1e-5f)

static const float COLOR[4] = { 2.0f, 0.75f, 0.95f, 0.07f };
static const float EXTENTS[3] = { 0.5714285f, 1.3125f, 104.99f };

static void keep_at(float x, int start, int fade_in, int fade_out, int duration)
{
    float c[3] = { x, 2.0f, 3.0f };
    sh_postfx_keep(c, start, fade_in, fade_out, duration, 100.0f, COLOR, EXTENTS, 0.3f);
}

int main(void)
{
    float v[13][4];
    const float none[3] = { 0.0f, 0.0f, 0.0f };
    printf("postfx_test\n");

    printf("-- which items are drawn: a cap and a radius\n");
    CHECK(sh_postfx_shape_drawn(COLOR, EXTENTS, 0.3f, 100.0f));
    CHECK(!sh_postfx_shape_drawn(COLOR, none, 0.3f, 100.0f));      /* the game's own items set no cap */
    CHECK(!sh_postfx_shape_drawn(COLOR, EXTENTS, 0.3f, 0.0f));

    printf("-- the envelope: 0 -> 1 over the fade in, held, 1 -> 0 over the fade out before the end\n");
    CHECK(NEAR(sh_postfx_envelope(1000, 100, 200, 1500, 999), 0.0f));
    CHECK(NEAR(sh_postfx_envelope(1000, 100, 200, 1500, 1050), 0.5f));
    CHECK(NEAR(sh_postfx_envelope(1000, 100, 200, 1500, 1200), 1.0f));
    CHECK(NEAR(sh_postfx_envelope(1000, 100, 200, 1500, 1400), 0.5f));
    CHECK(NEAR(sh_postfx_envelope(1000, 100, 200, 1500, 1500), 0.0f));
    CHECK(NEAR(sh_postfx_envelope(1000, 0, 0, 1500, 1000), 1.0f));

    printf("-- none on: the parameters say so and the program reads the plain scene\n");
    sh_postfx_forget();
    CHECK(sh_postfx_parms(0, 1.0f, v) == 0 && NEAR(v[0][0], 1.0f) && NEAR(v[0][1], 0.0f) && NEAR(v[0][2], 0.0f));
    keep_at(0.0f, 1000, 0, 0, 10);
    CHECK(sh_postfx_parms(2000, -1.0f, v) == 0 && NEAR(v[0][0], -1.0f) && NEAR(v[0][1], 0.0f));
    /* an item the game plays without a cap is not kept */
    {
        float c[3] = { 9.0f, 9.0f, 9.0f };
        sh_postfx_forget();
        sh_postfx_keep(c, 1000, 0, 0, 500, 100.0f, COLOR, none, 0.3f);
        CHECK(sh_postfx_parms(1100, 1.0f, v) == 0);
    }

    printf("-- one item: its layout; its end the longer of its duration and its fades\n");
    sh_postfx_forget();
    keep_at(1.0f, 1000, 100, 200, 250);         /* fades 300 > duration 250: ends at 1300 */
    CHECK(sh_postfx_parms(1050, 1.0f, v) == 1);
    CHECK(NEAR(v[0][1], 1.0f) && NEAR(v[0][2], 0.07f));
    CHECK(NEAR(v[1][0], 1.0f) && NEAR(v[1][1], 2.0f) && NEAR(v[1][2], 3.0f) && NEAR(v[1][3], 0.5f * 0.75f));
    CHECK(NEAR(v[2][0], 100.0f) && NEAR(v[2][1], 2.0f) && NEAR(v[2][2], 0.95f) && NEAR(v[2][3], 0.0f));
    CHECK(NEAR(v[3][0], 0.5714285f) && NEAR(v[3][1], 1.3125f) && NEAR(v[3][2], 104.99f) && NEAR(v[3][3], 0.3f));
    CHECK(sh_postfx_parms(1299, 1.0f, v) == 1 && sh_postfx_parms(1300, 1.0f, v) == 0);

    printf("-- every player view plays the same item: one blur\n");
    sh_postfx_forget();
    keep_at(1.0f, 1000, 0, 0, 500);
    keep_at(1.0f, 1000, 0, 0, 500);
    CHECK(sh_postfx_parms(1100, 1.0f, v) == 1);

    printf("-- six on: the newest four drawn, newest first\n");
    sh_postfx_forget();
    {
        int i;
        for (i = 0; i < 6; i++) keep_at((float)i, 1000 + i, 0, 0, 5000);
    }
    CHECK(sh_postfx_parms(2000, 1.0f, v) == 4);
    CHECK(NEAR(v[1][0], 5.0f) && NEAR(v[4][0], 4.0f) && NEAR(v[7][0], 3.0f) && NEAR(v[10][0], 2.0f));

    printf("-- nine kept: the oldest replaced\n");
    sh_postfx_forget();
    {
        int i;
        for (i = 0; i < 9; i++) keep_at((float)i, 1000 + i, 0, 0, i == 0 ? 5000 : 1);
    }
    /* only item 0 would still be on, and it was replaced by item 8 */
    CHECK(sh_postfx_parms(1500, 1.0f, v) == 0);

    printf("%s -- %d checks, %d failed\n", g_failed ? "FAILED" : "ok", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
