/* Render-world area and node counts of a SnapMap Play build.
 *
 * BuildRenderWorld partitions the placed modules' world bounds with a
 * disjoint-bounds kd tree, then halves every leaf cell on X and Y until each
 * piece is at most one cell size wide (512 units natively). Every piece is
 * one world area; every split is one area node. idRenderWorldLocal::ParseAreas
 * and ParseNodes refuse a map with more than 8192 of either, and that load
 * error leaves the native loading screen waiting forever. The render frontend
 * sizes its per-frame culling buffers by the same 8192, so the limit itself
 * cannot be raised; a map that needs more areas at 512 units is instead built
 * with a coarser cell size (world_area_cell.c). The count reproduces the
 * engine's float arithmetic on build 13954591. */
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

/* bounds holds one world box per placed module, in map order (min xyz, then
 * max xyz, exactly as GridWorldBounds returns it). cull is the value of the
 * engine cvar sm_testPortalSystem (default 1), which drops pieces outside the
 * owning module. Returns 1 and fills *out, or 0 for invalid input or a tree
 * the engine itself could not build. */
int sh_render_areas(const float (*bounds)[6], int n, int cull, sh_render_area_count *out);
/* The same count with cell size size (512 .. SH_RENDER_AREA_CELL_MAX). */
int sh_render_areas_cell(const float (*bounds)[6], int n, int cull, float size, sh_render_area_count *out);

/* Whether a count loads: at most 8192 areas and 8192 nodes. */
int sh_render_areas_fit(const sh_render_area_count *count);
/* Whether a count at a coarser size loads with SH_RENDER_AREA_HEADROOM to spare. */
int sh_render_areas_fit_coarse(const sh_render_area_count *count);

/* Choose the cell size for a layout: 512 whenever its count fits, else the
 * smallest of 1024, 2048, ... up to max_size whose count fits with headroom.
 * When none fits, cell is the largest size tried and fits is 0. max_size 512
 * only counts the native size. Returns 0 for a layout that cannot be counted. */
int sh_render_area_cell_choose(const float (*bounds)[6], int n, int cull, float max_size,
                               sh_render_cell_choice *out);

#endif
