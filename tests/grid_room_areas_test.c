/* Render-world area counts against the engine's ParseAreas/ParseNodes limits. */
#include "../src/backend/grid_room_areas.h"
#include "../src/backend/grid_room.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* A Grid Room's local box at a size, as the planner builds it. */
static void room_box(unsigned x, unsigned y, unsigned z, float out[6])
{
    sh_grid_size size = {SH_GRID_MODERN, {x, y, z}};
    sh_grid_warp warp;
    int k;
    assert(sh_grid_warp_init(&size, &warp));
    for (k = 0; k < 3; ++k) {
        out[k] = warp.axis[k].target[0];
        out[k + 3] = warp.axis[k].target[warp.axis[k].count - 1];
    }
}
/* The choice must be the smallest size that fits: the next smaller one
 * (if any) must not fit with the margin the choice requires. */
static void check_choice(const float (*boxes)[6], int n, const sh_render_cell_choice *c)
{
    sh_render_area_count smaller;
    assert(c->fits);
    if (c->cell == SH_RENDER_AREA_CELL) { assert(sh_render_areas_fit(&c->count)); return; }
    assert(sh_render_areas_fit_coarse(&c->count) && !sh_render_areas_fit(&c->native));
    assert(sh_render_areas_cell(boxes, n, 1, c->cell / 2, &smaller));
    assert(c->cell / 2 == SH_RENDER_AREA_CELL ? !sh_render_areas_fit(&smaller) : !sh_render_areas_fit_coarse(&smaller));
}
static sh_render_cell_choice choose_room(unsigned x, unsigned y, unsigned z)
{
    float box[1][6];
    sh_render_cell_choice c;
    room_box(x, y, z, box[0]);
    assert(sh_render_area_cell_choose((const float (*)[6])box, 1, 1, SH_RENDER_AREA_CELL_MAX, &c));
    check_choice((const float (*)[6])box, 1, &c);
    return c;
}
static sh_render_area_count one_room(unsigned x, unsigned y, unsigned z, int cull)
{
    float box[1][6];
    sh_render_area_count c;
    room_box(x, y, z, box[0]);
    assert(sh_render_areas((const float (*)[6])box, 1, cull, &c));
    return c;
}
int main(void)
{
    sh_render_area_count c;
    /* The reported map: one modern room at 64062 x 62846 x 32671 has 16512
     * world areas. The engine's single-module tree splits X at the room's own
     * minimum, so a zero-width slab (128 Y pieces) joins the 128 x 128 grid. */
    c = one_room(64062, 62846, 32671, 1);
    printf("64062 x 62846 x 32671: %lld areas, %lld nodes\n", c.areas, c.nodes);
    assert(c.areas == 16512 && c.nodes == 16511 && !sh_render_areas_fit(&c));
    c = one_room(64062, 62846, 32671, 0);
    assert(c.areas == 16512);
    /* The stock room: 5120 x 5120 has 16 x (16 + 1) areas. */
    c = one_room(5120, 5120, 3392, 1);
    printf("5120 x 5120 x 3392: %lld areas, %lld nodes\n", c.areas, c.nodes);
    assert(c.areas == 272 && c.nodes == 271 && sh_render_areas_fit(&c));
    /* Z is never subdivided. */
    assert(one_room(5120, 5120, 32671, 1).areas == 272);
    /* Single-room breakpoints: 64 x 65 fits, 128 x 65 and 64 x 129 do not,
     * 32 x 129 fits again. */
    {
        float box[1][6] = {{0, 0, 0, 32768, 32768, 100}};
        assert(sh_render_areas((const float (*)[6])box, 1, 1, &c) && c.areas == 64 * 65 && sh_render_areas_fit(&c));
        box[0][4] = 32769;
        assert(sh_render_areas((const float (*)[6])box, 1, 1, &c) && c.areas == 128 * 65 && !sh_render_areas_fit(&c));
        box[0][4] = 32768; box[0][3] = 32769;
        assert(sh_render_areas((const float (*)[6])box, 1, 1, &c) && c.areas == 64 * 129 && !sh_render_areas_fit(&c));
        box[0][3] = 65536; box[0][4] = 16384;
        assert(sh_render_areas((const float (*)[6])box, 1, 1, &c) && c.areas == 32 * 129 && sh_render_areas_fit(&c));
    }
    /* Two rooms: the tree splits X at 1024; the upper cell [1024,3072] keeps
     * three of its four pieces with culling (one lies wholly before the room),
     * all four without. */
    {
        float boxes[2][6] = {{0, 0, 0, 1024, 512, 100}, {2048, 0, 0, 3072, 512, 100}};
        assert(sh_render_areas((const float (*)[6])boxes, 2, 1, &c));
        assert(c.areas == 5 && c.nodes == 5);
        assert(sh_render_areas((const float (*)[6])boxes, 2, 0, &c));
        assert(c.areas == 6 && c.nodes == 5);
        /* Module order does not change the count. */
        {
            float swapped[2][6];
            memcpy(swapped[0], boxes[1], sizeof swapped[0]); memcpy(swapped[1], boxes[0], sizeof swapped[1]);
            assert(sh_render_areas((const float (*)[6])swapped, 2, 1, &c) && c.areas == 5 && c.nodes == 5);
        }
    }
    /* A large room beside a stock room counts only the pieces each touches. */
    {
        float boxes[2][6];
        room_box(30000, 30000, 8000, boxes[0]);
        room_box(5120, 5120, 3392, boxes[1]);
        boxes[1][0] += 20000; boxes[1][3] += 20000;
        assert(sh_render_areas((const float (*)[6])boxes, 2, 1, &c));
        printf("30000 room beside a stock room: %lld areas, %lld nodes\n", c.areas, c.nodes);
        /* Culled pieces still leave their split nodes: nodes exceed areas here. */
        assert(c.areas == 4228 && c.nodes == 4295);
    }
    /* Invalid input refuses. */
    {
        volatile float big = 1e30f;
        float box[1][6] = {{0, 0, 0, 1, 1, 1}};
        box[0][3] = big * big;
        assert(!sh_render_areas((const float (*)[6])box, 1, 1, &c));
        assert(!sh_render_areas(NULL, 1, 1, &c));
    }
    /* Cell size choice. The reported room needs 1024: 64 x (64 + 1) areas. */
    {
        sh_render_cell_choice k = choose_room(64062, 62846, 32671);
        printf("64062 x 62846 x 32671: cell %.0f, %lld areas, %lld nodes (512: %lld)\n", k.cell, k.count.areas, k.count.nodes, k.native.areas);
        assert(k.cell == 1024 && k.count.areas == 4160 && k.count.nodes == 4159);
        assert(k.native.areas == 16512 && k.native.nodes == 16511);
        /* Rooms that fit keep 512 with the native count. */
        k = choose_room(5120, 5120, 3392);
        assert(k.cell == 512 && k.count.areas == 272 && k.native.areas == 272);
        k = choose_room(32768, 32768, 32671);
        assert(k.cell == 512 && k.count.areas == 4160);
        /* One unit past the single-room breakpoint: 1024 gives 32 x 65. */
        {
            float box[1][6] = {{0, 0, 0, 32769, 32768, 100}};
            assert(sh_render_area_cell_choose((const float (*)[6])box, 1, 1, SH_RENDER_AREA_CELL_MAX, &k));
            check_choice((const float (*)[6])box, 1, &k);
            assert(k.cell == 1024 && k.count.areas == 32 * 65 && k.native.areas == 64 * 129);
        }
        /* Without the redirect only 512 is counted: the big room does not fit. */
        {
            float box[1][6];
            room_box(64062, 62846, 32671, box[0]);
            assert(sh_render_area_cell_choose((const float (*)[6])box, 1, 1, SH_RENDER_AREA_CELL, &k));
            assert(k.cell == 512 && !k.fits && k.count.areas == 16512 && k.coarsest.areas == 16512);
            assert(!sh_render_area_cell_choose((const float (*)[6])box, 1, 1, 300, &k));
            assert(!sh_render_area_cell_choose((const float (*)[6])box, 1, 1, 1e9f, &k));
            assert(!sh_render_areas_cell((const float (*)[6])box, 1, 1, 256, &c));
        }
        /* The margin a coarser size needs. */
        c.areas = 7680; c.nodes = 7680; assert(sh_render_areas_fit_coarse(&c));
        c.areas = 7681; assert(!sh_render_areas_fit_coarse(&c) && sh_render_areas_fit(&c));
        c.areas = 7680; c.nodes = 7681; assert(!sh_render_areas_fit_coarse(&c));
    }
    /* Many rooms: a 4 x 4 block of 16000 x 15000 rooms filling the unlocked
     * map (+/-32000) and the same block with a tall room stacked on one. */
    {
        float boxes[17][6];
        sh_render_cell_choice k;
        int i, j, n = 0;
        for (i = 0; i < 4; ++i) for (j = 0; j < 4; ++j, ++n) {
            boxes[n][0] = -32000.0f + 16000.0f * i; boxes[n][3] = boxes[n][0] + 16000;
            boxes[n][1] = -30000.0f + 15000.0f * j; boxes[n][4] = boxes[n][1] + 15000;
            boxes[n][2] = 0; boxes[n][5] = 4000;
        }
        assert(sh_render_area_cell_choose((const float (*)[6])boxes, n, 1, SH_RENDER_AREA_CELL_MAX, &k));
        printf("4 x 4 block of 16000 x 15000 rooms: cell %.0f, %lld areas, %lld nodes (512: %lld / %lld)\n",
               k.cell, k.count.areas, k.count.nodes, k.native.areas, k.native.nodes);
        check_choice((const float (*)[6])boxes, n, &k);
        assert(k.cell > 512);
        memcpy(boxes[n], boxes[0], sizeof boxes[n]); boxes[n][2] = boxes[0][5]; boxes[n][5] = boxes[n][2] + 20000; ++n;
        assert(sh_render_area_cell_choose((const float (*)[6])boxes, n, 1, SH_RENDER_AREA_CELL_MAX, &k));
        printf("with a room stacked on one: cell %.0f, %lld areas, %lld nodes (512: %lld / %lld)\n",
               k.cell, k.count.areas, k.count.nodes, k.native.areas, k.native.nodes);
        check_choice((const float (*)[6])boxes, n, &k);
    }
    puts("grid_room_areas_test: ok");
    return 0;
}
