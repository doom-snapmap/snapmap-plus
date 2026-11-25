#include <math.h>
#include <stdio.h>
#include <string.h>
#define SH_RIBBON_MOTION_TEST
#include "../src/backend/ribbonmotion.c"

static int failures, called, same_edge, original_args;
static const float *sent_a, *sent_b, *sent_v;
static float got_a[3], got_b[3], got_v[3];

#define CHECK(x) do { if (!(x)) { puts("FAIL " #x); ++failures; } } while (0)
static int close3(const float a[3], const float b[3])
{
    return fabsf(a[0]-b[0]) < .0015f && fabsf(a[1]-b[1]) < .0015f && fabsf(a[2]-b[2]) < .0015f;
}

static void observed(void *ribbon, int time, const float *a, const float *b, const float *axis,
                     const float *speed, const float *colour)
{
    (void)ribbon; (void)time; (void)axis; (void)colour;
    ++called;
    same_edge = a == b;
    original_args = a == sent_a && b == sent_b && speed == sent_v;
    memcpy(got_a, a, sizeof got_a);
    memcpy(got_b, b, sizeof got_b);
    memcpy(got_v, speed, sizeof got_v);
}

int main(void)
{
    __declspec(align(8)) unsigned char decl[0x120] = {0}, instance[0x50] = {0};
    sh_ribbon_noise m = { .01905f, SH_RIBBON_NOISE, 1.0f, { .191f/.01905f, .100f/.01905f, .85f/.01905f, 1 } };
    const float centre[] = {-460, -1944, 80}, moving[] = {10, 100, 20}, a[] = {-460,-1945,80}, b[] = {-460,-1943,80};
    const float normal[] = {4.018821f,-1.065205f,-9.534164f}, initial[] = {-1.886745f,-.546484f,-3.280557f};
    float offset[3], drift[3], zero[3] = {0}, expected[3], edge[3];
    const char *name = "mods/gxrib005/swipe";
    uint32_t spawn = 0x1234;
    int i;
    CHECK(sh_ribbon_motion_owned(name));
    CHECK(!sh_ribbon_motion_owned("ai/tyrant/swipe"));
    CHECK(!sh_ribbon_motion_owned("mods/"));
    CHECK(!sh_ribbon_motion_owned("mods/gxrib005/"));
    CHECK(!sh_ribbon_motion_owned("mods/wrong/smoke"));
    CHECK(sh_ribbon_motion_sample(&m, centre, spawn, 375, 0, offset, drift));
    CHECK(close3(offset, zero) && close3(drift, normal));
    m.marker = SH_RIBBON_NOISE_AT_BIRTH;
    CHECK(sh_ribbon_motion_sample(&m, centre, spawn, 375, 1, offset, drift));
    CHECK(close3(offset, initial) && close3(drift, initial));
    m.velocity = 2;
    CHECK(sh_ribbon_motion_sample(&m, centre, spawn, 375, 0, offset, drift));
    for (i = 0; i < 3; ++i) expected[i] = normal[i] * 2;
    CHECK(close3(offset, normal) && close3(drift, expected));

    original_birth = observed;
    memcpy(instance + 0x10, &spawn, sizeof spawn);
    *(int *)(instance + 0x1c) = 1;
    *(void **)(instance + 0x48) = decl;
    memcpy(decl + 8, &name, sizeof name);
    memcpy(decl + 0xf4, &m, sizeof m);
    sent_a = a; sent_b = b; sent_v = moving;
    ribbon_birth(instance, 375, a, b, NULL, moving, NULL);
    CHECK(called == 1 && !same_edge && !original_args);
    for (i = 0; i < 3; ++i) {
        expected[i] = moving[i] + 2*normal[i];
        edge[i] = a[i] + normal[i];
    }
    CHECK(close3(got_v, expected) && close3(got_a, edge));
    for (i = 0; i < 3; ++i) edge[i] = b[i] + normal[i];
    CHECK(close3(got_b, edge));
    sent_b = sent_a = centre;
    m.marker = SH_RIBBON_NOISE;
    memcpy(decl + 0xf4, &m, sizeof m);
    ribbon_birth(instance, 375, centre, centre, NULL, moving, NULL);
    CHECK(called == 2 && same_edge && close3(got_a, centre) && close3(got_v, expected));

    name = "ribbon/stock";
    memcpy(decl + 8, &name, sizeof name);
    ribbon_birth(instance, 375, centre, centre, NULL, moving, NULL);
    CHECK(called == 3 && original_args);
    name = "mods/gxrib005/ordinary";
    memcpy(decl + 8, &name, sizeof name);
    m.marker = 0;
    memcpy(decl + 0xf4, &m, sizeof m);
    ribbon_birth(instance, 375, centre, centre, NULL, moving, NULL);
    CHECK(called == 4 && original_args);
    m.marker = SH_RIBBON_NOISE;
    m.magic[2] = NAN;
    memcpy(decl + 0xf4, &m, sizeof m);
    ribbon_birth(instance, 375, centre, centre, NULL, moving, NULL);
    CHECK(called == 5 && original_args);
    puts(failures ? "ribbonmotion_test: FAIL" : "ribbonmotion_test: PASS");
    return failures != 0;
}
