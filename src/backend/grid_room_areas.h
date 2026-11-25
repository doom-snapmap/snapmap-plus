/* Render-world area and node counts of a SnapMap Play build. BuildRenderWorld splits the module bounds with a
 * disjoint-bounds kd tree, then halves each leaf on X and Y to at most one cell size (512 natively); each piece is an
 * area and each split a node, and ParseAreas/ParseNodes refuse a map over 8192 of either (the loading screen waits
 * forever), so a larger map is built with a coarser cell (world_area_cell.c). Reproduces build 13954591's floats. */
#ifndef SH_GRID_ROOM_AREAS_H
#define SH_GRID_ROOM_AREAS_H

#define SH_RENDER_AREA_MAX 8192
#define SH_RENDER_NODE_MAX 8192
#define SH_RENDER_AREA_CELL 512.0f
/* Past 65536 every piece of the largest reachable map is already one area. */
#define SH_RENDER_AREA_CELL_MAX 65536.0f
/* A coarser size is chosen only with this margin below both limits. */
#define SH_RENDER_AREA_HEADROOM 512

typedef struct sh_render_area_count {
    long long areas, nodes;
} sh_render_area_count;

typedef struct sh_render_cell_choice {
    float cell;                     /* the size Play should use */
    int fits;                       /* the count at cell loads */
    sh_render_area_count count;     /* at cell */
    sh_render_area_count native;    /* at 512 */
    sh_render_area_count coarsest;  /* at the largest size tried */
} sh_render_cell_choice;

/* Whether a count loads: at most 8192 areas and 8192 nodes. */
int sh_render_areas_fit(const sh_render_area_count *count);
/* Whether a count at a coarser size loads with SH_RENDER_AREA_HEADROOM to spare. */
int sh_render_areas_fit_coarse(const sh_render_area_count *count);

/* Chooses the cell size for a layout: 512 whenever its count fits, else the smallest of 1024, 2048, ... up to
 * max_size whose count fits with headroom. When none fits, cell is the largest size tried and fits is 0; max_size
 * 512 only counts the native size. Returns 0 for a layout that cannot be counted. */
int sh_render_area_cell_choose(const float (*bounds)[6], int n, int cull, float max_size,
                               sh_render_cell_choice *out);

#endif
