/* nav_overlay.h -- the editor's placement grid (stock render model "<module>/<leaf>_aas_<class>.bmodel"), cut where
 * the bake cut the module floor: its floor triangles are rewritten to cover the bake's pieces (largest piece in
 * place, the rest appended after the original areas) and everything else is kept. */
#ifndef SNAPMAP_PLUS_NAV_OVERLAY_H
#define SNAPMAP_PLUS_NAV_OVERLAY_H
#include <stddef.h>

/* A convex polygon in any winding, xyz per corner. */
typedef struct sh_ov_polygon {
    int count;
    const double (*c)[3];
} sh_ov_polygon;

/* One cut floor area: the ground it covered before the cut, and the convex
 * pieces of that ground the bake kept. */
typedef struct sh_ov_cut {
    sh_ov_polygon whole;
    const sh_ov_polygon *pieces;
    int piece_count;
} sh_ov_cut;

/* A cut applies to a triangle lying within this height of the cut area. The
 * stock floors sit at their area's height; a module's upper surfaces are
 * thousands of units away. */
#define SH_OV_Z_TOLERANCE 48.0

/* Split one triangle around the cuts into replacement triangles on its plane and winding; returns their count.
 * *touched is 0 when no cut overlaps (the triangle is written once); returns -1 for invalid input or short `cap`. */
int sh_nav_overlay_cut_triangle(const double tri[3][3], const sh_ov_cut *cuts, int cut_count,
                                double (*out)[3][3], int cap, int *touched);

typedef struct sh_nav_overlay_stats {
    int cut_areas;          /* shipped floor areas the bake cut */
    int pieces;             /* pieces kept from them */
    int triangles_before;   /* in the stock model */
    int triangles_after;    /* in the rewritten model */
    int triangles_replaced; /* stock triangles that lay on a cut area */
    int vertices_before, vertices_after;
} sh_nav_overlay_stats;

/* Rewrite a stock placement-display model for one class from its shipped navigation and the bake's cut; `first_area`
 * is the first area after the appended floor pieces. Returns 1 with a HeapAlloc'd model in *out, 0 when nothing was
 * cut (keep the stock model), -1 when an input does not parse or the result would not fit the format. */
int sh_nav_overlay_build(const unsigned char *model, size_t model_len,
                         const unsigned char *shipped, size_t shipped_len,
                         const unsigned char *cut, size_t cut_len, unsigned first_area,
                         unsigned char **out, size_t *out_len, sh_nav_overlay_stats *stats);

/* The triangle count of a .bmodel, or -1 when it does not parse. */
int sh_nav_overlay_triangle_count(const unsigned char *model, size_t model_len);

#endif
