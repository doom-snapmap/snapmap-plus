/* A placed module's orientation: one of eight codes (four quarter turns, then
 * four reflections), in the engine's order. Row o maps a module-local (x, y)
 * to world: world.x = M[o][0]*x + M[o][1]*y, world.y = M[o][2]*x + M[o][3]*y.
 * Every M[o] is orthogonal, so its inverse is its transpose. The one table
 * the navigation bake, its snapshot and Fly to read. */
#ifndef SH_MODULE_ORIENTATION_H
#define SH_MODULE_ORIENTATION_H

static const int SH_MODULE_ORIENT[8][4] = {
    { 1, 0, 0, 1 }, { 0, -1, 1, 0 }, { -1, 0, 0, -1 }, { 0, 1, -1, 0 },
    { -1, 0, 0, 1 }, { 0, 1, 1, 0 }, { 1, 0, 0, -1 }, { 0, -1, -1, 0 }
};

/* Module-local (x, y) to world (out_x, out_y), translation aside. An unknown code is 0. */
static void sh_module_orient(int o, float x, float y, float *out_x, float *out_y)
{
    const int *m = SH_MODULE_ORIENT[o >= 0 && o < 8 ? o : 0];
    *out_x = (float)m[0] * x + (float)m[1] * y;
    *out_y = (float)m[2] * x + (float)m[3] * y;
}

/* The inverse: world (x, y), translation removed, to module-local. */
static void sh_module_orient_inverse(int o, float x, float y, float *out_x, float *out_y)
{
    const int *m = SH_MODULE_ORIENT[o >= 0 && o < 8 ? o : 0];
    *out_x = (float)m[0] * x + (float)m[2] * y;
    *out_y = (float)m[1] * x + (float)m[3] * y;
}

#endif
