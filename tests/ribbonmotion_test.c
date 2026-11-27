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

static struct knot { int at; float x, y; } knots[160];
static int knot_count;

static void record(void *instance, int at, const float *p, const float *q, const float *axis,
                   const float *velocity, const float *colour)
{
    (void)axis; (void)velocity; (void)colour;
    CHECK(p == q && knot_count < 160);
    if (knot_count < 160) knots[knot_count++] = (struct knot){at, p[0], p[1]};
    char *data = instance;
    int head = (*(int *)(data + 0x20) + 1) & 127;
    *(int *)(data + 0x20) = head;
    ++*(int *)(data + 0x1c);
    memcpy(data + 0x60 + head*0x78, p, sizeof(float)*3);
    *(int *)(data + 0xac + head*0x78) = at;
}

static void trail(int frame_ms, float mode, float period, float count, int until)
{
    __declspec(align(8)) char decl[0x120] = {0}, instance[0x3c68] = {0};
    *(const char **)(decl + 8) = "mods/gxrib005/sampled";
    *(const char **)(instance + 0x48) = decl;
    *(float *)(decl + 0xd4) = mode;
    *(float *)(decl + 0xdc) = period;
    *(float *)(decl + 0xe0) = count;
    *(float *)(decl + 0xe4) = mode == SH_RIBBON_DISTANCE_KNOTS ? 50 : 5;
    *(int *)(instance + 0x10) = 1000;
    *(int *)(instance + 0x2c) = -1;
    *(int *)(instance + 0x20) = -1;
    original_birth = record;
    knot_count = 0;
    for (int t = 0;; t += frame_ms) {
        if (t > until) t = until;
        float point[3] = { .1f*t, .0003f*t*t, 0 }, velocity[3] = {0};
        ribbon_birth(instance, 1000+t, point, point, NULL, velocity, NULL);
        *(int *)(instance + 0x2c) = 1000+t;
        memcpy(instance + 0x30, point, sizeof point);
        if (t == until) break;
    }
}

static double camera_range;
static double camera(const float *point) { (void)point; return camera_range; }

typedef struct { void **methods; float origin[3]; } test_view;
typedef struct { void **methods; int ready; fx_views views; } test_world;
static unsigned char game_ready(void *w) { return (unsigned char)((test_world *)w)->ready; }
static const fx_views *game_views(void *w) { return &((test_world *)w)->views; }
static void view_pose(void *v, float *origin, float *axis)
{
    memcpy(origin, ((test_view *)v)->origin, 3*sizeof(float));
    memset(axis, 0, 9*sizeof(float));
}

static void camera_views(void)
{
    void *game_methods[106] = {0}, *view_methods[5] = {0};
    test_view nearby = {view_methods, {8,5,4}}, distant = {view_methods, {200,0,0}};
    void *both[] = {&distant, NULL, &nearby};
    test_world game = {game_methods, 1, {both,3,3}};
    float point[] = {5,1,4};
    game_methods[0x38/8] = (void *)game_ready; game_methods[0x348/8] = (void *)game_views;
    view_methods[0x20/8] = (void *)view_pose;
    CHECK(distance_to_views(&game, point) == 5);
    game.ready = 0;
    CHECK(distance_to_views(&game, point) == 0);
    game.ready = 1; game.views.count = 0;
    CHECK(distance_to_views(&game, point) == 0);
}

static void sampling(void)
{
    for (int n = 13; n <= 33; n += 20) {
        struct knot reference[33];
        trail(9, SH_RIBBON_TIMED_KNOTS, 500, (float)n, 720);
        CHECK(knot_count == n);
        memcpy(reference, knots, (size_t)n * sizeof *knots);
        trail(34, SH_RIBBON_TIMED_KNOTS, 500, (float)n, 720);
        CHECK(knot_count == n);
        for (int i = 0; i < n; ++i) {
            double t = 500.0*i/(n-1);
            CHECK(knots[i].at == 1000+(int)round(t) && knots[i].at == reference[i].at);
            CHECK(fabs(knots[i].x - .1*t) < .001);
            CHECK(fabs(knots[i].y - .0003*t*t) < .09);
            CHECK(fabs(knots[i].y - reference[i].y) < .09);
        }
        trail(11, SH_RIBBON_TIMED_KNOTS, 500, (float)n, 400);
        CHECK(knot_count == (n == 13 ? 10 : 26));
    }
    view_distance = camera;
    /* Counts differ with the source's frame-by-frame accumulation. Each insertion fills up to the new pose. */
    const int frames[] = {9, 34};
    const int near9[] = {0,54,108,153,198,243,279,315,351,387}, near34[] = {0,102,204,272,340,400};
    for (int i = 0; i < 2; ++i) {
        const int *expected = i ? near34 : near9;
        camera_range = 0;
        trail(frames[i], SH_RIBBON_DISTANCE_KNOTS, 15.748032f, 1049.86877f, 400);
        CHECK(knot_count == (i ? 6 : 10));
        for (int j = 0; j < knot_count; ++j) CHECK(knots[j].at == 1000+expected[j]);
        camera_range = 25;
        trail(frames[i], SH_RIBBON_DISTANCE_KNOTS, 15.748032f, 1049.86877f, 400);
        CHECK(knot_count == (i ? 1 : 2));
        if (!i) CHECK(knots[1].at == 1288);
        camera_range = 100;
        trail(frames[i], SH_RIBBON_DISTANCE_KNOTS, 15.748032f, 1049.86877f, 400);
        CHECK(knot_count == (i ? 1 : 2));
        if (!i) CHECK(knots[1].at == 1387);
    }
    trail(400, SH_RIBBON_DISTANCE_KNOTS, 1, 0, 400);
    CHECK(knot_count == 17 && knots[8].at == 1200 && knots[16].at == 1400);
    view_distance = NULL;
    beam_segment strip[12] = {0};
    render_list rows = {(char *)strip, 3, 12};
    const float y[] = {0, 2, 2, 0};
    for (int i = 0; i < 3; ++i) {
        strip[i].start[0] = (float)i; strip[i].end[0] = (float)i+1;
        strip[i].start[1] = y[i]; strip[i].end[1] = y[i+1];
        strip[i].u[0] = (float)i/3; strip[i].u[1] = (float)(i+1)/3;
    }
    smooth_beam(&rows, 4);
    CHECK(rows.count == 12 && strip[6].start[1] == 2.25f);
    CHECK(strip[0].u[0] == 0 && strip[11].u[1] == 1 && strip[6].u[0] == .5f);
    for (int i = 0; i < 11; ++i) CHECK(close3(strip[i].end, strip[i+1].start));
}

static __declspec(align(8)) char model[0xeb0], beams[2 * 0x3a28], nodes[2 * 0x74];
static int produce;

static void draw(void *instance, int time, void *target, const float *a, const float *b)
{
    render_list *list = (render_list *)(model + 0xe90);
    render_list *fresh = (render_list *)(beams + 0x3a28);
    (void)instance; (void)time; (void)a; (void)b;
    CHECK(target == model);
    if (!produce) return;
    ++list->count;
    fresh->data = nodes;
    fresh->count = fresh->capacity = 2;
    *(float *)(nodes + 0x4c) = 4;
    *(float *)(nodes + 0x74 + 0x4c) = 8;
}

static void widths(void)
{
    __declspec(align(8)) char decl[0x120] = {0}, instance[0x50] = {0};
    __declspec(align(8)) char earlier_node[0x74] = {0};
    const char *name = "mods/gxrib005/width";
    render_list *list = (render_list *)(model + 0xe90);
    sh_ribbon_noise *p = (sh_ribbon_noise *)(decl + 0xf4);
    *(const char **)(decl + 8) = name;
    *(void **)(instance + 0x48) = decl;
    *(int *)(instance + 0x10) = 4000;
    p->marker = SH_RIBBON_WIDTH_ENVELOPE;
    p->magic[1] = 1000;
    p->magic[3] = 1.5f;
    original_draw = draw;
    produce = 1;
    list->data = beams;
    list->capacity = 2;
    render_list *earlier = (render_list *)beams;
    earlier->data = earlier_node;
    earlier->count = earlier->capacity = 1;
    *(float *)(earlier_node + 0x4c) = 3;
    const int times[] = {4000, 4250, 4500, 5000, 6000};
    const float factors[] = {0, .375f, .75f, 1.5f, 1.5f};
    for (int i = 0; i < 5; ++i) {
        list->count = 1;
        ribbon_draw(instance, times[i], model, NULL, NULL);
        CHECK(*(float *)(nodes + 0x4c) == 4 * factors[i]);
        CHECK(*(float *)(nodes + 0x74 + 0x4c) == 8 * factors[i]);
        CHECK(*(float *)(earlier_node + 0x4c) == 3);
    }
    produce = 0;
    list->count = 1;
    ribbon_draw(instance, 4000, model, NULL, NULL);
    CHECK(*(float *)(nodes + 0x4c) == 6);
    produce = 1;
    *(const char **)(decl + 8) = "ribbon/stock";
    ribbon_draw(instance, 4000, model, NULL, NULL);
    CHECK(*(float *)(nodes + 0x4c) == 4);
    list->count = 1;
    *(const char **)(decl + 8) = name;
    p->marker = 0;
    ribbon_draw(instance, 4000, model, NULL, NULL);
    CHECK(*(float *)(nodes + 0x4c) == 4);
    list->count = 1;
    p->marker = SH_RIBBON_WIDTH_ENVELOPE;
    p->magic[3] = NAN;
    ribbon_draw(instance, 4000, model, NULL, NULL);
    CHECK(*(float *)(nodes + 0x4c) == 4);
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

static void owner_fade(void)
{
    __declspec(align(8)) char decl[0x120] = {0}, instance[0x3c68] = {0};
    render_list *list = (render_list *)(model + 0xe90);
    beam_segment *strip = (beam_segment *)nodes;
    *(const char **)(decl + 8) = "mods/gxrib005/fade";
    *(const char **)(instance + 0x48) = decl;
    *(int *)(instance + 0x10) = 1000;
    *(float *)(decl + 0xd4) = SH_RIBBON_TIMED_KNOTS;
    *(float *)(decl + 0xe4) = 1;
    *(float *)(decl + 0xe8) = 400;
    *(float *)(decl + 0xec) = -750;
    original_draw = draw;
    list->data = beams;
    list->capacity = 2;
    produce = 1;
    const int times[] = {1399,1400,1500,1575,1749,1750,1900};
    const int expected[] = {255,255,182,128,1,0,0};
    for (int i = 0; i < 7; ++i) {
        list->count = 1;
        memset(nodes, 0x36, sizeof nodes);
        ribbon_draw(instance, times[i], model, NULL, NULL);
        for (int j = 0; j < 2; ++j) {
            for (int k = 0; k < 4; ++k) {
                CHECK(strip[j].tangent[k][0] == expected[i]);
                CHECK(strip[j].tangent[k][1] == 0x36);
            }
            CHECK(strip[j].colour[3] == 0x36);
        }
    }
    *(int *)instance = 1;
    list->count = 1;
    ribbon_draw(instance, 1200, model, NULL, NULL);
    CHECK(strip[0].tangent[0][0] == 0);
    *(int *)instance = 0;
    *(int *)(instance + 0x10) = 3000;
    list->count = 1;
    ribbon_draw(instance, 3000, model, NULL, NULL);
    CHECK(strip[0].tangent[0][0] == 255);
    *(float *)(decl + 0xd8) = 11;
    *(float *)(decl + 0xe8) = 350;
    *(float *)(decl + 0xec) = -1050;
    list->count = 1;
    ribbon_draw(instance, 3700, model, NULL, NULL);
    CHECK(strip[0].tangent[0][0] == 191);
}

int main(void)
{
    owner_fade();
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
    widths();
    camera_views();
    sampling();
    puts(failures ? "ribbonmotion_test: FAIL" : "ribbonmotion_test: PASS");
    return failures != 0;
}
