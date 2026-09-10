/* nav_regions_test.c -- the author's navigation regions, read out of a map.
 *
 * Every map here is SYNTHESIZED, member by member, to the shapes a real map
 * uses: this product ships no game bytes and a test may not either. What the
 * tests pin is the part that cannot be seen by looking at one volume -- that a
 * ticked box becomes the rectangle at its TOP face, that a box nobody ticked
 * is left alone, and that ownership comes from `instanceEntities` and nothing
 * else. Two instances of one module hold their volumes at identical
 * module-local coordinates, so an attribution bug does not look wrong on one
 * instance; it looks like the other instance's geometry.
 *
 * The malformed cases are not politeness. The input is a downloaded map, so a
 * truncated, unbalanced or lying document has to come back empty rather than
 * read off the end of the buffer.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nav_regions.h"

static int g_failed;

#define CHECK(expr) do {                                                         \
    if (!(expr)) {                                                               \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++;                                                              \
    }                                                                            \
} while (0)

static int near_f(float a, float b)
{
    float d = a - b;
    return d < 0.001f && d > -0.001f;
}

/* ---- reading a region's quad ------------------------------------------
 *
 * A region is four corners now, not a rect, so what used to be a member read is
 * a min or max over the corners. The values these return for an UPRIGHT volume
 * are exactly what the old x0/y1/top_z members held, which is what lets the
 * pre-existing cases below assert the same numbers they always did. */
static float quad_min_x(const sh_nav_region *r)
{
    float v = r->c[0][0]; int i;
    for (i = 1; i < 4; i++) if (r->c[i][0] < v) v = r->c[i][0];
    return v;
}
static float quad_max_x(const sh_nav_region *r)
{
    float v = r->c[0][0]; int i;
    for (i = 1; i < 4; i++) if (r->c[i][0] > v) v = r->c[i][0];
    return v;
}
static float quad_min_y(const sh_nav_region *r)
{
    float v = r->c[0][1]; int i;
    for (i = 1; i < 4; i++) if (r->c[i][1] < v) v = r->c[i][1];
    return v;
}
static float quad_max_y(const sh_nav_region *r)
{
    float v = r->c[0][1]; int i;
    for (i = 1; i < 4; i++) if (r->c[i][1] > v) v = r->c[i][1];
    return v;
}
static float quad_extent_x(const sh_nav_region *r) { return quad_max_x(r) - quad_min_x(r); }
static float quad_extent_y(const sh_nav_region *r) { return quad_max_y(r) - quad_min_y(r); }
/* The face is planar, so any corner's z is the surface height for a level one. */
static float quad_top_z(const sh_nav_region *r) { return r->c[0][2]; }

/* ==================================================================== */
/* building a map                                                        */
/* ==================================================================== */

typedef struct blob { char *p; size_t len, cap; } blob;

static void bopen(blob *b)
{
    b->cap = 4096;
    b->len = 0;
    b->p = (char *)malloc(b->cap);
    if (b->p) b->p[0] = '\0';
}

static void bclose(blob *b)
{
    free(b->p);
    b->p = NULL;
    b->len = b->cap = 0;
}

static void bput(blob *b, const char *fmt, ...)
{
    for (;;) {
        va_list ap;
        int n;
        if (!b->p) return;
        va_start(ap, fmt);
        n = _vsnprintf_s(b->p + b->len, b->cap - b->len, _TRUNCATE, fmt, ap);
        va_end(ap);
        if (n >= 0) { b->len += (size_t)n; return; }
        b->cap *= 2;
        b->p = (char *)realloc(b->p, b->cap);
    }
}

static const char *MODULE      = "ind_dlc/ind_totally_blank_room_4x";
static const char *MODULE_DECL = "maps/modules/ind_dlc/ind_totally_blank_room_4x.decl";
static const char *INHERIT     = "snapmaps/volume/blocking";

static void put_instance(blob *b, int first, const char *decl,
                         double ox, double oy, double oz, int orientation)
{
    bput(b, "%s{\"difficultyOffset\":0,\"environmentName\":\"snapmap/base\",\"layerMask\":1,"
            "\"moduleName\":\"%s\",\"orientation\":%d,"
            "\"origin\":{\"x\":%g,\"y\":%g,\"z\":%g,\"~type\":\"idVec3\"},"
            "\"restrictionMask\":0,\"~type\":\"idSnapInstance\"}",
         first ? "" : ",", decl, orientation, ox, oy, oz);
}

static void put_entity(blob *b, int first, int uid, const char *inherit, const char *edit)
{
    bput(b, "%s{\"displayName\":\"\",\"entityDef\":{\"className\":\"idVolume_Blocking\","
            "\"inherit\":\"%s\",\"name\":\"\",\"state\":{\"edit\":{%s}},"
            "\"targetType\":\"idDeclEntityDef\",\"~type\":\"idDeclEntityDef\"},"
            "\"layerMask\":1,\"pinned\":true,\"uniqueId\":%d,\"~type\":\"idSnapEntity\"}",
         first ? "" : ",", inherit, edit, uid);
}

/* A blocking volume's `edit` object. `renderModelInfo` carries its own `size`
 * in a real map, and it is not the box -- writing a decoy one here is what
 * makes the member lookups prove they are scoped to `clipModelInfo`. */
static void edit_box(char *out, size_t cap, const char *flags, const char *type,
                     double cx, double cy, double cz,
                     double sx, double sy, double sz)
{
    _snprintf_s(out, cap, _TRUNCATE,
        "%s\"clipModelInfo\":{%s\"size\":{\"x\":%g,\"y\":%g,\"z\":%g}},"
        "\"isOpaque\":false,"
        "\"renderModelInfo\":{\"model\":\"industrial/panel.hotspot\","
        "\"scale\":{\"x\":9999,\"y\":9999,\"z\":9999},"
        "\"size\":{\"x\":9999,\"y\":9999,\"z\":9999}},"
        "\"spawnPosition\":{\"x\":%g,\"y\":%g,\"z\":%g}",
        flags, type, sx, sy, sz, cx, cy, cz);
}

/* Like edit_box, plus a raw `spawnOrientation` fragment. NULL means the member
 * is absent, which the reader must treat as the identity. */
static void edit_box_or(char *out, size_t cap, const char *flags, const char *type,
                        double cx, double cy, double cz,
                        double sx, double sy, double sz, const char *orient)
{
    _snprintf_s(out, cap, _TRUNCATE,
        "%s\"clipModelInfo\":{%s\"size\":{\"x\":%g,\"y\":%g,\"z\":%g}},"
        "\"isOpaque\":false,"
        "\"renderModelInfo\":{\"model\":\"industrial/panel.hotspot\","
        "\"scale\":{\"x\":9999,\"y\":9999,\"z\":9999},"
        "\"size\":{\"x\":9999,\"y\":9999,\"z\":9999}},"
        "%s%s\"spawnPosition\":{\"x\":%g,\"y\":%g,\"z\":%g}",
        flags, type, sx, sy, sz, orient ? orient : "", orient ? "," : "", cx, cy, cz);
}

static char *map_of(const char *instances, const char *entities,
                    const char *key_values, const char *values, size_t *out_len)
{
    blob b;
    bopen(&b);
    bput(&b, "{\"entities\":[%s],"
             "\"instanceEntities\":{\"keyValues\":[%s],\"values\":[%s],"
             "\"~type\":\"idIndexMultimap\"},"
             "\"instances\":[%s],"
             "\"variables\":{\"allocCount\":[0,0,0,0,0,0,0,0,0,16],\"string\":[],"
             "\"~type\":\"idSnapVariables\"},\"~type\":\"idSnapMap\"}",
         entities, key_values, values, instances);
    *out_len = b.len;
    return b.p;
}

/* ==================================================================== */
/* the volume an author ticked                                           */
/* ==================================================================== */

/* One instance, one ticked box. The rectangle is centred on spawnPosition in x
 * and y, and its z is the TOP of the box -- spawnPosition.z is the bottom. */
/* One instance, one ticked Blocking Box, correctly attributed. Attribution
 * through `instanceEntities` is not optional: without it the keep filter drops
 * the region and every assertion reads zero. `orient` is a raw
 * `spawnOrientation` fragment, or NULL for none.
 *
 * The caller frees the returned document; the blobs are closed here. */
static char *map_with_volume(double sx, double sy, double sz,
                             double px, double py, double pz,
                             const char *orient, size_t *out_n)
{
    blob inst, ents;
    char edit[2048];
    char *json;

    bopen(&inst);
    bopen(&ents);
    put_instance(&inst, 1, MODULE_DECL, 0, 0, 0, 0);
    edit_box_or(edit, sizeof edit, "\"affectsNavmesh\":true,\"blockDemons\":true,", "",
                px, py, pz, sx, sy, sz, orient);
    put_entity(&ents, 1, 7, INHERIT, edit);
    json = map_of(inst.p, ents.p, "0,1,1", "7", out_n);
    bclose(&inst);
    bclose(&ents);
    return json;
}

static void test_one_volume(void)
{
    blob inst, ents;
    char edit[1024];
    char *json;
    size_t n;
    sh_nav_map m;

    bopen(&inst);
    bopen(&ents);
    put_instance(&inst, 1, MODULE_DECL, 1152, -2944, 0, 3);
    edit_box(edit, sizeof edit, "\"affectsNavmesh\":true,\"blockDemons\":true,", "",
             100, 200, 64, 200, 400, 128);
    put_entity(&ents, 1, 7, INHERIT, edit);
    json = map_of(inst.p, ents.p, "0,1,1", "7", &n);

    CHECK(sh_nav_regions_read(json, n, &m) == 1);
    CHECK(m.instance_count == 1);
    CHECK(strcmp(m.instances[0].module, MODULE) == 0);
    CHECK(near_f(m.instances[0].origin[0], 1152.0f));
    CHECK(near_f(m.instances[0].origin[1], -2944.0f));
    CHECK(near_f(m.instances[0].origin[2], 0.0f));
    CHECK(m.instances[0].orientation == 3);
    CHECK(m.instances[0].region_count == 1);
    CHECK(m.region_count == 1);
    CHECK(m.truncated == 0);
    if (m.region_count == 1) {
        CHECK(near_f(quad_min_x(&m.regions[0]), 0.0f));
        CHECK(near_f(quad_max_x(&m.regions[0]), 200.0f));
        CHECK(near_f(quad_min_y(&m.regions[0]), 0.0f));
        CHECK(near_f(quad_max_y(&m.regions[0]), 400.0f));
        CHECK(near_f(quad_top_z(&m.regions[0]), 192.0f));   /* bottom 64 + height 128 */
        CHECK(m.regions[0].instance == 0);
        CHECK(m.regions[0].block_demons == 1);
        CHECK(m.regions[0].entity == 0);
    }
    CHECK(sh_nav_regions_nth_instance(&m, MODULE, 0) == 0);
    CHECK(sh_nav_regions_nth_instance(&m, MODULE, 1) == -1);
    CHECK(sh_nav_regions_nth_instance(&m, "ind_dlc/some_other_room", 0) == -1);

    free(json);
    bclose(&inst);
    bclose(&ents);
}

/* Only `affectsNavmesh` makes a region. A map full of ordinary blocking
 * volumes -- which is every map published so far -- has none. */
static void test_marker_required(void)
{
    blob inst, ents;
    char edit[1024];
    char *json;
    size_t n;
    sh_nav_map m;

    bopen(&inst);
    bopen(&ents);
    put_instance(&inst, 1, MODULE_DECL, 0, 0, 0, 0);
    edit_box(edit, sizeof edit, "\"blockDemons\":true,", "", 0, 0, 0, 64, 64, 8);
    put_entity(&ents, 1, 1, INHERIT, edit);                 /* absent is false */
    edit_box(edit, sizeof edit, "\"affectsNavmesh\":false,\"blockDemons\":true,", "",
             0, 0, 0, 64, 64, 8);
    put_entity(&ents, 0, 2, INHERIT, edit);
    edit_box(edit, sizeof edit, "\"affectsNavmesh\":true,", "", 0, 0, 0, 64, 64, 8);
    put_entity(&ents, 0, 3, INHERIT, edit);
    /* and an entity that is not a blocking volume at all, ticked or not */
    edit_box(edit, sizeof edit, "\"affectsNavmesh\":true,", "", 0, 0, 0, 64, 64, 8);
    put_entity(&ents, 0, 4, "snapmaps/prop/static", edit);
    json = map_of(inst.p, ents.p, "0,4,4", "1,2,3,4", &n);

    CHECK(sh_nav_regions_read(json, n, &m) == 1);
    CHECK(m.region_count == 1);
    if (m.region_count == 1) {
        CHECK(m.regions[0].entity == 2);
        /* blockDemons absent is false: a demon falls straight through this box,
         * and the caller is the one that decides what to do about it */
        CHECK(m.regions[0].block_demons == 0);
    }

    free(json);
    bclose(&inst);
    bclose(&ents);
}

/* Only a box has a top face, and only a box with area has one at all. */
static void test_box_shape(void)
{
    static const char *TICKED = "\"affectsNavmesh\":true,\"blockDemons\":true,";
    blob inst, ents;
    char edit[1024];
    char *json;
    size_t n;
    sh_nav_map m;

    bopen(&inst);
    bopen(&ents);
    put_instance(&inst, 1, MODULE_DECL, 0, 0, 0, 0);
    /* 0: the type spelled out, which is what absent means */
    edit_box(edit, sizeof edit, TICKED, "\"type\":\"CLIPMODEL_BOX\",",
             0, 0, 0, 64, 64, 8);
    put_entity(&ents, 1, 1, INHERIT, edit);
    /* 1: a shape with no top face we could derive */
    edit_box(edit, sizeof edit, TICKED, "\"type\":\"CLIPMODEL_CYLINDER\",",
             0, 0, 0, 64, 64, 8);
    put_entity(&ents, 0, 2, INHERIT, edit);
    /* 2 and 3: degenerate in x, then in y */
    edit_box(edit, sizeof edit, TICKED, "", 0, 0, 0, 0, 64, 8);
    put_entity(&ents, 0, 3, INHERIT, edit);
    edit_box(edit, sizeof edit, TICKED, "", 0, 0, 0, 64, -256, 8);
    put_entity(&ents, 0, 4, INHERIT, edit);
    /* 4: the editor omits a vector component that is zero */
    put_entity(&ents, 0, 5, INHERIT,
               "\"affectsNavmesh\":true,\"blockDemons\":true,"
               "\"clipModelInfo\":{\"size\":{\"x\":512,\"y\":256,\"z\":16}},"
               "\"spawnPosition\":{\"z\":32}");
    json = map_of(inst.p, ents.p, "0,5,5", "1,2,3,4,5", &n);

    CHECK(sh_nav_regions_read(json, n, &m) == 1);
    CHECK(m.region_count == 2);
    if (m.region_count == 2) {
        CHECK(m.regions[0].entity == 0);
        CHECK(m.regions[1].entity == 4);
        CHECK(near_f(quad_min_x(&m.regions[1]), -256.0f));
        CHECK(near_f(quad_max_x(&m.regions[1]), 256.0f));
        CHECK(near_f(quad_min_y(&m.regions[1]), -128.0f));
        CHECK(near_f(quad_max_y(&m.regions[1]), 128.0f));
        CHECK(near_f(quad_top_z(&m.regions[1]), 48.0f));
    }

    free(json);
    bclose(&inst);
    bclose(&ents);
}

/* ==================================================================== */
/* whose volume is it                                                    */
/* ==================================================================== */

/* Two instances of THE SAME module, one ticked volume each. Their coordinates
 * are module-local and identical, so `instanceEntities` is the only thing that
 * can tell the two apart -- and the Nth instance of a module is the Nth one in
 * the instance array, which is the order BuildAAS opens them in. */
static void test_two_instances_of_one_module(void)
{
    blob inst, ents;
    char edit[1024];
    char *json;
    size_t n;
    sh_nav_map m;

    bopen(&inst);
    bopen(&ents);
    put_instance(&inst, 1, MODULE_DECL, 0, 0, 0, 0);
    put_instance(&inst, 0, MODULE_DECL, 2048, 0, 0, 2);
    edit_box(edit, sizeof edit, "\"affectsNavmesh\":true,\"blockDemons\":true,", "",
             0, 0, 0, 128, 128, 64);
    put_entity(&ents, 1, 11, INHERIT, edit);
    put_entity(&ents, 0, 22, INHERIT, edit);
    /* 2 instances -> 4 keyValues: a bucket each, then the orphan bucket */
    json = map_of(inst.p, ents.p, "0,1,2,2", "11,22", &n);

    CHECK(sh_nav_regions_read(json, n, &m) == 1);
    CHECK(m.instance_count == 2);
    CHECK(sh_nav_regions_nth_instance(&m, MODULE, 0) == 0);
    CHECK(sh_nav_regions_nth_instance(&m, MODULE, 1) == 1);
    CHECK(sh_nav_regions_nth_instance(&m, MODULE, 2) == -1);
    CHECK(sh_nav_regions_nth_instance(&m, MODULE, -1) == -1);
    CHECK(m.instances[1].orientation == 2);
    CHECK(near_f(m.instances[1].origin[0], 2048.0f));
    CHECK(m.region_count == 2);
    CHECK(m.instances[0].region_count == 1);
    CHECK(m.instances[1].region_count == 1);
    if (m.region_count == 2) {
        CHECK(m.regions[0].entity == 0 && m.regions[0].instance == 0);
        CHECK(m.regions[1].entity == 1 && m.regions[1].instance == 1);
        /* module-local, so the second instance's origin is NOT applied */
        CHECK(near_f(quad_min_x(&m.regions[1]), -64.0f) && near_f(quad_max_x(&m.regions[1]), 64.0f));
    }
    free(json);

    /* the same map with the buckets holding the other entity: attribution
     * follows the multimap, not the order the entities happen to be in */
    json = map_of(inst.p, ents.p, "0,1,2,2", "22,11", &n);
    CHECK(sh_nav_regions_read(json, n, &m) == 1);
    CHECK(m.region_count == 2);
    if (m.region_count == 2) {
        CHECK(m.regions[0].entity == 0 && m.regions[0].instance == 1);
        CHECK(m.regions[1].entity == 1 && m.regions[1].instance == 0);
    }
    free(json);

    /* Incomplete ownership refuses the snapshot, including an earlier bucket
     * that happened to be readable. No partial per-module bake is published. */
    json = map_of(inst.p, ents.p, "0,1", "11,22", &n);
    CHECK(sh_nav_regions_read(json, n, &m) == 1);
    CHECK(m.invalid_geometry == 1);
    CHECK(m.region_count == 0);
    CHECK(m.instances[0].region_count == 0);
    CHECK(m.instances[1].region_count == 0);
    free(json);

    /* Duplicate ownership, decreasing offsets and truncated value arrays
     * cannot assign one physical solid to the wrong instance. */
    json = map_of(inst.p, ents.p, "0,1,2,2", "11,11", &n);
    CHECK(sh_nav_regions_read(json,n,&m));CHECK(m.invalid_geometry);free(json);
    json = map_of(inst.p, ents.p, "0,2,1,2", "11,22", &n);
    CHECK(sh_nav_regions_read(json,n,&m));CHECK(m.invalid_geometry);free(json);
    json = map_of(inst.p, ents.p, "0,1,2,2", "11", &n);
    CHECK(sh_nav_regions_read(json,n,&m));CHECK(m.invalid_geometry);free(json);
    json = map_of(inst.p, ents.p, "0,1,2,2", "11.5,22", &n);
    CHECK(sh_nav_regions_read(json,n,&m));CHECK(m.invalid_geometry);free(json);
    json = map_of(inst.p, ents.p, "0,1,2,2", "11,99", &n);
    CHECK(sh_nav_regions_read(json,n,&m));CHECK(m.invalid_geometry);free(json);

    bclose(&inst);
    bclose(&ents);
}

/* The last bucket is the orphan bucket: entities that belong to no instance.
 * A ticked volume in it is skipped, because guessing its owner by coordinates
 * is exactly what the test above shows cannot work. */
static void test_orphan_bucket(void)
{
    blob inst, ents;
    char edit[1024];
    char *json;
    size_t n;
    sh_nav_map m;

    bopen(&inst);
    bopen(&ents);
    put_instance(&inst, 1, MODULE_DECL, 0, 0, 0, 0);
    edit_box(edit, sizeof edit, "\"affectsNavmesh\":true,\"blockDemons\":true,", "",
             0, 0, 0, 128, 128, 64);
    put_entity(&ents, 1, 5, INHERIT, edit);     /* instance 0's bucket */
    put_entity(&ents, 0, 6, INHERIT, edit);     /* the orphan bucket */
    put_entity(&ents, 0, 9, INHERIT, edit);     /* in no bucket at all */
    json = map_of(inst.p, ents.p, "0,1,2", "5,6", &n);

    CHECK(sh_nav_regions_read(json, n, &m) == 1);
    CHECK(m.instance_count == 1);
    CHECK(m.region_count == 1);
    CHECK(m.instances[0].region_count == 1);
    CHECK(m.truncated == 0);                    /* a skip is not a cap */
    if (m.region_count == 1) CHECK(m.regions[0].entity == 0);

    free(json);
    bclose(&inst);
    bclose(&ents);
}

/* An instance whose moduleName is not a module path keeps its slot -- the
 * multimap addresses instances by position -- but nothing can be baked for it,
 * so its volumes are dropped and it answers to no module name. */
static void test_unnameable_instance(void)
{
    blob inst, ents;
    char edit[1024];
    char *json;
    size_t n;
    sh_nav_map m;

    bopen(&inst);
    bopen(&ents);
    put_instance(&inst, 1, "maps/modules/no_category.decl", 0, 0, 0, 0);
    put_instance(&inst, 0, "somewhere/else/entirely", 0, 0, 0, 0);
    put_instance(&inst, 0, MODULE_DECL, 0, 0, 0, 0);
    edit_box(edit, sizeof edit, "\"affectsNavmesh\":true,\"blockDemons\":true,", "",
             0, 0, 0, 128, 128, 64);
    put_entity(&ents, 1, 1, INHERIT, edit);
    put_entity(&ents, 0, 2, INHERIT, edit);
    json = map_of(inst.p, ents.p, "0,1,1,2,2", "1,2", &n);

    CHECK(sh_nav_regions_read(json, n, &m) == 1);
    CHECK(m.instance_count == 3);
    CHECK(m.instances[0].module[0] == '\0');
    CHECK(m.instances[1].module[0] == '\0');
    CHECK(strcmp(m.instances[2].module, MODULE) == 0);
    CHECK(sh_nav_regions_nth_instance(&m, MODULE, 0) == 2);
    CHECK(sh_nav_regions_nth_instance(&m, "", 0) == -1);
    CHECK(m.region_count == 1);                 /* instance 0's volume is dropped */
    if (m.region_count == 1) CHECK(m.regions[0].instance == 2);

    free(json);
    bclose(&inst);
    bclose(&ents);
}

/* ==================================================================== */
/* the caps                                                              */
/* ==================================================================== */

static void test_truncation(void)
{
    const int volumes = SH_NAVR_MAX_REGIONS + 8;
    const int placed  = SH_NAVR_MAX_INSTANCES + 4;
    blob inst, ents, kv, vals;
    char edit[1024];
    char *json;
    size_t n;
    sh_nav_map m;
    int i;

    /* more ticked volumes than the region table holds */
    bopen(&inst);
    bopen(&ents);
    bopen(&vals);
    put_instance(&inst, 1, MODULE_DECL, 0, 0, 0, 0);
    edit_box(edit, sizeof edit, "\"affectsNavmesh\":true,\"blockDemons\":true,", "",
             0, 0, 0, 64, 64, 8);
    for (i = 0; i < volumes; i++) {
        put_entity(&ents, i == 0, i + 1, INHERIT, edit);
        bput(&vals, "%s%d", i ? "," : "", i + 1);
    }
    bopen(&kv);
    bput(&kv, "0,%d,%d", volumes, volumes);
    json = map_of(inst.p, ents.p, kv.p, vals.p, &n);

    CHECK(sh_nav_regions_read(json, n, &m) == 1);
    CHECK(m.region_count == SH_NAVR_MAX_REGIONS);
    CHECK(m.instances[0].region_count == SH_NAVR_MAX_REGIONS);
    CHECK(m.truncated == 1);

    free(json);
    bclose(&inst);
    bclose(&ents);
    bclose(&kv);
    bclose(&vals);

    /* more placed instances than the instance table holds */
    bopen(&inst);
    for (i = 0; i < placed; i++) put_instance(&inst, i == 0, MODULE_DECL, 0, 0, 0, 0);
    json = map_of(inst.p, "", "0,0", "", &n);

    CHECK(sh_nav_regions_read(json, n, &m) == 1);
    CHECK(m.instance_count == SH_NAVR_MAX_INSTANCES);
    CHECK(m.region_count == 0);
    CHECK(m.truncated == 1);
    CHECK(sh_nav_regions_nth_instance(&m, MODULE, SH_NAVR_MAX_INSTANCES - 1) ==
          SH_NAVR_MAX_INSTANCES - 1);
    CHECK(sh_nav_regions_nth_instance(&m, MODULE, SH_NAVR_MAX_INSTANCES) == -1);

    free(json);
    bclose(&inst);
}

/* ==================================================================== */
/* a stranger's map                                                      */
/* ==================================================================== */

/* `out` is fully overwritten even when the document is refused, so a caller
 * that ignores the return value still reads an empty map rather than whatever
 * was on its stack. */
static void refused(const char *json, size_t len)
{
    sh_nav_map m;
    memset(&m, 0xAA, sizeof m);
    CHECK(sh_nav_regions_read(json, len, &m) == 0);
    CHECK(m.instance_count == 0);
    CHECK(m.region_count == 0);
    CHECK(m.truncated == 0);
}

#define REFUSED(s) refused((s), strlen(s))

static void test_malformed(void)
{
    static const char *good_ish = "{\"instances\":[],\"entities\":[]}";
    blob inst, ents;
    char edit[1024];
    char *json;
    size_t n, cut;
    sh_nav_map m;

    refused(NULL, 16);
    refused(good_ish, 0);
    REFUSED("");
    REFUSED("{");
    REFUSED("}");
    REFUSED("[1,2,3]");
    REFUSED("{\"entities\":[]}");                  /* no instance list: not a map */
    REFUSED("{\"instances\":{}}");                 /* ...and it has to be a list */
    REFUSED("{\"instances\":[\"");                 /* a string that never closes */
    REFUSED("{\"instances\":[{\"moduleName\":\"maps/modules/a/b.decl\"}");

    /* a real map, cut off mid-document, which is what a truncated download or
     * a half-written file looks like */
    bopen(&inst);
    bopen(&ents);
    put_instance(&inst, 1, MODULE_DECL, 0, 0, 0, 0);
    edit_box(edit, sizeof edit, "\"affectsNavmesh\":true,\"blockDemons\":true,", "",
             0, 0, 0, 128, 128, 64);
    put_entity(&ents, 1, 1, INHERIT, edit);
    json = map_of(inst.p, ents.p, "0,1,1", "1", &n);
    for (cut = 1; cut < n; cut += 37) refused(json, cut);
    CHECK(sh_nav_regions_read(json, n, &m) == 1);      /* ...and whole, it still reads */
    CHECK(m.region_count == 1);
    free(json);

    /* a map that parses but whose multimap says nothing usable: every volume is
     * unattributed, which is empty rather than wrong */
    json = map_of(inst.p, ents.p, "\"a\",\"b\",\"c\"", "1", &n);
    CHECK(sh_nav_regions_read(json, n, &m) == 1);
    CHECK(m.instance_count == 1);
    CHECK(m.region_count == 0);
    free(json);

    json = map_of(inst.p, ents.p, "", "", &n);
    CHECK(sh_nav_regions_read(json, n, &m) == 1);
    CHECK(m.region_count == 0);
    free(json);

    /* keyValues that runs off the end of values, and one that runs backwards */
    json = map_of(inst.p, ents.p, "0,900,900", "1", &n);
    CHECK(sh_nav_regions_read(json, n, &m) == 1);
    CHECK(m.region_count == 1);
    free(json);

    json = map_of(inst.p, ents.p, "9,1,1", "1", &n);
    CHECK(sh_nav_regions_read(json, n, &m) == 1);
    CHECK(m.region_count == 0);
    free(json);

    /* no multimap at all */
    {
        blob b;
        bopen(&b);
        bput(&b, "{\"instances\":[%s],\"entities\":[%s],\"~type\":\"idSnapMap\"}",
             inst.p, ents.p);
        CHECK(sh_nav_regions_read(b.p, b.len, &m) == 1);
        CHECK(m.instance_count == 1);
        CHECK(m.region_count == 0);
        bclose(&b);
    }

    CHECK(sh_nav_regions_nth_instance(NULL, MODULE, 0) == -1);
    CHECK(sh_nav_regions_nth_instance(&m, NULL, 0) == -1);

    bclose(&inst);
    bclose(&ents);
}

/* ==================================================================== */
/* the live editor                                                       */
/* ==================================================================== */

/* Pressing Play does not serialize the map, so the bytes above are what the
 * author saved and NOT what they are looking at. These tests stand in for the
 * editor: a table of entities the engine would serialize on request, driven
 * through the same two callbacks the backend passes down. No game is involved
 * and no game bytes are here -- each document is written member by member to
 * the shape the engine's reflection emits for one idSnapEntity.
 *
 * THE ID IS THE uniqueId, NOT the position in the map JSON's entities array.
 * The editor's entity table is sparse and indexed by uniqueId: measured live
 * 2026-09-08, an 11-entity map whose uniqueIds ran 56..69 produced an editor
 * table of highWater 70, and the marked Blocking Box -- entities[3], uniqueId
 * 62 -- answered at live id 62. These tests therefore place each document at its
 * uniqueId, which for the shared two-box map below is 11 and 22. An earlier
 * revision of both this file and nav_regions.c assumed the array index, which is
 * why a volume ticked in the editor never survived to the bake.
 *
 * The live document is still not obliged to CARRY `uniqueId` as a member: it is
 * addressed by the id it answers to, not by what it says about itself, and
 * several documents below leave the member out entirely to keep that honest. */

#define LIVE_MAX 640

typedef struct live_editor {
    const char *json[LIVE_MAX];     /* what the engine would hand back; NULL: no such entity */
    int         refuse[LIVE_MAX];   /* live, but serializing it fails */
    int         overrun[LIVE_MAX];  /* ...and one that reports more than it wrote */
    int         queried[LIVE_MAX];  /* how many times get_json was asked for this id */
    int         valid_calls;
} live_editor;

static int live_valid(int id, void *ctx)
{
    live_editor *e = (live_editor *)ctx;
    e->valid_calls++;
    if (id < 0 || id >= LIVE_MAX) return 0;
    return e->json[id] != NULL;
}

static int live_json(int id, char *out, int cap, void *ctx)
{
    live_editor *e = (live_editor *)ctx;
    size_t n;

    if (id < 0 || id >= LIVE_MAX || cap <= 0) return 0;
    e->queried[id]++;
    if (!e->json[id] || e->refuse[id]) return 0;
    n = strlen(e->json[id]);
    if ((int)n >= cap) return 0;
    /* The contract is a LENGTH, not a string: fill the rest with an unbalanced
     * brace so anything read past the reported length fails loudly instead of
     * happening to work. */
    memset(out, '{', (size_t)cap);
    memcpy(out, e->json[id], n);
    if (e->overrun[id]) return cap + 64;
    return (int)n;
}

/* An editor that is there but can serialize nothing -- the shape of a live
 * surface that is not actually readable. */
static int live_all_valid(int id, void *ctx) { (void)id; (void)ctx; return 1; }
static int live_all_fail(int id, char *out, int cap, void *ctx)
{
    (void)id; (void)ctx;
    if (cap > 0) out[0] = '\0';
    return 0;
}

/* One entity as the engine serializes it. `uid` < 0 leaves `uniqueId` out. */
static char *live_entity(const char *inherit, const char *edit, int uid)
{
    blob b;
    bopen(&b);
    bput(&b, "{\"displayName\":\"\",\"entityDef\":{\"className\":\"idVolume_Blocking\","
             "\"inherit\":\"%s\",\"name\":\"\",\"state\":{\"edit\":{%s}},"
             "\"targetType\":\"idDeclEntityDef\",\"~type\":\"idDeclEntityDef\"},"
             "\"layerMask\":1,\"pinned\":true", inherit, edit);
    if (uid >= 0) bput(&b, ",\"uniqueId\":%d", uid);
    bput(&b, ",\"~type\":\"idSnapEntity\"}");
    return b.p;
}

static char *live_box(const char *flags, double cx, double cy, double cz,
                      double sx, double sy, double sz)
{
    char edit[1024];
    edit_box(edit, sizeof edit, flags, "", cx, cy, cz, sx, sy, sz);
    return live_entity(INHERIT, edit, -1);
}

static const char *TICKED  = "\"affectsNavmesh\":true,\"blockDemons\":true,";
static const char *UNTICKED = "\"blockDemons\":true,";

/* The map every test below starts from: one instance, two blocking boxes, and
 * only the first of them ticked when it was read. */
static char *two_box_map(sh_nav_map *m)
{
    blob inst, ents;
    char edit[1024];
    char *json;
    size_t n;

    bopen(&inst);
    bopen(&ents);
    put_instance(&inst, 1, MODULE_DECL, 0, 0, 0, 0);
    edit_box(edit, sizeof edit, TICKED, "", 0, 0, 0, 128, 128, 64);
    put_entity(&ents, 1, 11, INHERIT, edit);
    edit_box(edit, sizeof edit, UNTICKED, "", 100, 200, 64, 200, 400, 128);
    put_entity(&ents, 0, 22, INHERIT, edit);
    json = map_of(inst.p, ents.p, "0,2,2", "11,22", &n);
    CHECK(sh_nav_regions_read(json, n, m) == 1);
    CHECK(m->region_count == 1);
    bclose(&inst);
    bclose(&ents);
    return json;
}

/* The bug this exists for: the author ticks "AI Navigation" on a box that was
 * already in the map and presses Play. Nothing is saved, so the map bytes still
 * say the box is ordinary -- and the volume has to be picked up anyway, at its
 * top face, attributed to the instance the map said owns it. */
static void test_live_tick_this_session(void)
{
    sh_nav_map m;
    live_editor e;
    char *json = two_box_map(&m);
    char *live0 = live_box(TICKED, 0, 0, 0, 128, 128, 64);
    char *live1 = live_box(TICKED, 100, 200, 64, 200, 400, 128);

    memset(&e, 0, sizeof e);
    e.json[11] = live0;
    e.json[22] = live1;

    CHECK(sh_nav_regions_refresh_live(&m, 22, live_valid, live_json, &e) == 2);
    CHECK(m.region_count == 2);
    CHECK(m.instances[0].region_count == 2);
    CHECK(m.truncated == 0);
    CHECK(strcmp(m.instances[0].module, MODULE) == 0);   /* the instance is untouched */
    if (m.region_count == 2) {
        CHECK(m.regions[1].entity == 22);
        CHECK(m.regions[1].instance == 0);
        CHECK(near_f(quad_min_x(&m.regions[1]), 0.0f));
        CHECK(near_f(quad_max_x(&m.regions[1]), 200.0f));
        CHECK(near_f(quad_min_y(&m.regions[1]), 0.0f));
        CHECK(near_f(quad_max_y(&m.regions[1]), 400.0f));
        CHECK(near_f(quad_top_z(&m.regions[1]), 192.0f));       /* bottom 64 + height 128 */
        CHECK(m.regions[1].block_demons == 1);
    }

    free(json); free(live0); free(live1);
}

/* ...and the other direction. A volume the map says is marked, unticked since,
 * is not navigation any more -- whichever way the tick is spelled live. */
static void test_live_untick_this_session(void)
{
    sh_nav_map m;
    live_editor e;
    char *json = two_box_map(&m);
    char *live0 = live_box("\"affectsNavmesh\":false,\"blockDemons\":true,", 0, 0, 0, 128, 128, 64);
    char *live1 = live_box(UNTICKED, 100, 200, 64, 200, 400, 128);

    memset(&e, 0, sizeof e);
    e.json[11] = live0;
    e.json[22] = live1;

    CHECK(sh_nav_regions_refresh_live(&m, 22, live_valid, live_json, &e) == 0);
    CHECK(m.region_count == 0);
    CHECK(m.instances[0].region_count == 0);
    CHECK(m.instance_count == 1);
    /* the dropped region is gone, not merely uncounted */
    CHECK(m.regions[0].entity == 0 && m.regions[0].instance == 0 &&
          near_f(quad_top_z(&m.regions[0]), 0.0f));

    free(json); free(live0); free(live1);
}

/* PRESSING PLAY MUST NOT UNMARK THE MAP.
 *
 * The live entity array becomes the play session's the moment a playtest starts,
 * so entities keep answering and not one of them is a Blocking Box. That is a
 * surface that has stopped showing us this map's volumes -- NOT an author who
 * unticked them, because unticking leaves the box there without the marker.
 *
 * Reading it as the latter deleted every mark on every Play, and since the
 * commit also collapses the bake plan the feature stayed off for the rest of the
 * session: measured live 2026-09-06, editor "1 volume(s) ... ready" -> Play "no
 * volume in this map is marked for AI navigation". */
static void test_live_no_volumes_keeps_map_marks(void)
{
    sh_nav_map m, before;
    live_editor e;
    char *json = two_box_map(&m);
    /* entities that answer, none of them a Blocking Box */
    char *live0 = live_entity("snapmaps/logic/counter", "{}", -1);
    char *live1 = live_entity("snapmaps/spawners/encounter", "{}", -1);

    before = m;
    memset(&e, 0, sizeof e);
    e.json[11] = live0;
    e.json[22] = live1;

    CHECK(sh_nav_regions_refresh_live(&m, 22, live_valid, live_json, &e) == -1);
    CHECK(memcmp(&before, &m, sizeof m) == 0);
    CHECK(m.region_count == 1);

    free(json); free(live0); free(live1);
}

/* MARKS WE CANNOT PLACE ARE NOT EVIDENCE THE MAP HAS NONE.
 *
 * Attribution is by the id the map load recorded. When the live surface answers
 * about a different id space -- which is what a playtest does -- the marked
 * volumes are found and then every one of them falls out at the owner lookup.
 * Committing that leaves an empty table, collapses the bake plan, and the
 * feature is off for the session. The load already placed these correctly.
 *
 * Live: "the live editor read dropped 1 module(s) -- scanned 70 entity id(s),
 * found 1 marked volume(s)". */
static void test_live_marked_but_unattributable_keeps_map_marks(void)
{
    sh_nav_map m, before;
    live_editor e;
    char *json = two_box_map(&m);
    /* a marked Blocking Box at an id the load never saw -> no owner */
    char *live9 = live_box(TICKED, 0, 0, 0, 128, 128, 64);

    before = m;
    memset(&e, 0, sizeof e);
    e.json[9] = live9;

    CHECK(sh_nav_regions_refresh_live(&m, 9, live_valid, live_json, &e) == -1);
    CHECK(memcmp(&before, &m, sizeof m) == 0);
    CHECK(m.region_count == 1);

    free(json); free(live9);
}

/* The other side of that rule, so the guard above cannot be widened into "never
 * unmark anything": a surface that DOES show us the Blocking Boxes is entitled
 * to say none of them is marked any more. */
static void test_live_unmarked_volumes_still_clear(void)
{
    sh_nav_map m;
    live_editor e;
    char *json = two_box_map(&m);
    char *live0 = live_box(UNTICKED, 0, 0, 0, 128, 128, 64);
    char *live1 = live_box(UNTICKED, 100, 200, 64, 200, 400, 128);

    memset(&e, 0, sizeof e);
    e.json[11] = live0;
    e.json[22] = live1;

    CHECK(sh_nav_regions_refresh_live(&m, 22, live_valid, live_json, &e) == 0);
    CHECK(m.region_count == 0);

    free(json); free(live0); free(live1);
}

/* ABSENT IS FALSE, live exactly as in the map: an untouched volume carries no
 * `affectsNavmesh` member at all, and reading that as "ticked" would turn every
 * blocking box in the map into navigation. */
static void test_live_absent_marker_is_false(void)
{
    sh_nav_map m;
    live_editor e;
    char *json = two_box_map(&m);
    char *live0 = live_box("\"blockDemons\":true,", 0, 0, 0, 128, 128, 64);
    char *live1 = live_box("", 100, 200, 64, 200, 400, 128);

    memset(&e, 0, sizeof e);
    e.json[11] = live0;
    e.json[22] = live1;

    CHECK(sh_nav_regions_refresh_live(&m, 22, live_valid, live_json, &e) == 0);
    CHECK(m.region_count == 0);

    free(json); free(live0); free(live1);
}

/* `blockDemons` rides along, and absent is false there too. A volume demons
 * fall through is not a floor, and the caller is the one that decides what to
 * do about it -- so the flag has to arrive as the author left it. */
static void test_live_block_demons(void)
{
    sh_nav_map m;
    live_editor e;
    char *json = two_box_map(&m);
    char *live0 = live_box("\"affectsNavmesh\":true,", 0, 0, 0, 128, 128, 64);
    char *live1 = live_box("\"affectsNavmesh\":true,\"blockDemons\":true,",
                           100, 200, 64, 200, 400, 128);

    memset(&e, 0, sizeof e);
    e.json[11] = live0;
    e.json[22] = live1;

    CHECK(sh_nav_regions_refresh_live(&m, 22, live_valid, live_json, &e) == 2);
    CHECK(m.region_count == 2);
    if (m.region_count == 2) {
        CHECK(m.regions[0].entity == 11 && m.regions[0].block_demons == 0);
        CHECK(m.regions[1].entity == 22 && m.regions[1].block_demons == 1);
    }

    free(json); free(live0); free(live1);
}

/* A volume the load pass never saw has no owner anywhere. Coordinates cannot
 * rescue it -- they are module-local -- so it is skipped, and the skip is
 * visible as the gap between what was found and what was kept. */
static void test_live_volume_without_attribution(void)
{
    sh_nav_map m;
    live_editor e;
    char *json = two_box_map(&m);
    char *live0 = live_box(TICKED, 0, 0, 0, 128, 128, 64);
    char *live1 = live_box(TICKED, 100, 200, 64, 200, 400, 128);
    /* placed this session: the map that was read has no uniqueId 33 at all */
    char *live2 = live_box(TICKED, 0, 0, 0, 64, 64, 8);

    memset(&e, 0, sizeof e);
    e.json[11] = live0;
    e.json[22] = live1;
    e.json[33] = live2;

    CHECK(sh_nav_regions_refresh_live(&m, 33, live_valid, live_json, &e) == 3);
    CHECK(m.region_count == 2);              /* found three, kept the two it can place */
    CHECK(m.instances[0].region_count == 2);
    CHECK(m.truncated == 0);                 /* a skip is not a cap */
    if (m.region_count == 2) {
        CHECK(m.regions[0].entity == 11);
        CHECK(m.regions[1].entity == 22);
    }
    free(json); free(live0); free(live1); free(live2);

    /* the same refusal for an id the map did have, holding something that was
     * not a blocking volume when it was read: the multimap said nothing about
     * a volume there, so neither does this */
    {
        blob inst, ents;
        char edit[1024];
        size_t n;

        bopen(&inst);
        bopen(&ents);
        put_instance(&inst, 1, MODULE_DECL, 0, 0, 0, 0);
        edit_box(edit, sizeof edit, TICKED, "", 0, 0, 0, 128, 128, 64);
        put_entity(&ents, 1, 11, INHERIT, edit);
        put_entity(&ents, 0, 22, "snapmaps/prop/static", edit);
        json = map_of(inst.p, ents.p, "0,2,2", "11,22", &n);
        CHECK(sh_nav_regions_read(json, n, &m) == 1);
        CHECK(m.region_count == 1);

        memset(&e, 0, sizeof e);
        e.json[11] = live0 = live_box(TICKED, 0, 0, 0, 128, 128, 64);
        e.json[22] = live1 = live_box(TICKED, 100, 200, 64, 200, 400, 128);
        CHECK(sh_nav_regions_refresh_live(&m, 22, live_valid, live_json, &e) == 2);
        CHECK(m.region_count == 1);
        if (m.region_count == 1) CHECK(m.regions[0].entity == 11);

        free(json); free(live0); free(live1);
        bclose(&inst);
        bclose(&ents);
    }
}

/* An id the editor rejects is not an entity, and asking it to serialize one is
 * how a scan over a stale id range faults. It is not asked. */
static void test_live_valid_gates_the_scan(void)
{
    sh_nav_map m;
    live_editor e;
    char *json = two_box_map(&m);
    char *live1 = live_box(TICKED, 100, 200, 64, 200, 400, 128);

    memset(&e, 0, sizeof e);
    e.json[22] = live1;     /* every id but 22 is not live */

    CHECK(sh_nav_regions_refresh_live(&m, 30, live_valid, live_json, &e) == 1);
    CHECK(e.queried[0] == 0);
    CHECK(e.queried[11] == 0);
    CHECK(e.queried[22] == 1);
    CHECK(e.queried[2] == 0 && e.queried[29] == 0);
    CHECK(m.region_count == 1);
    if (m.region_count == 1) CHECK(m.regions[0].entity == 22);
    free(json);

    /* and an entity whose serializer reports more than it wrote is refused
     * rather than read past the end of what it handed over.
     *
     * Refusing it leaves NO legible Blocking Box on the surface, so the map
     * keeps the marks it loaded with: an entity we could not read is not an
     * entity that told us it is unmarked. Same reasoning as the no-surface case
     * below, and the reason a Play cannot unmark a map. */
    json = two_box_map(&m);
    memset(&e, 0, sizeof e);
    e.json[22] = live1;
    e.overrun[22] = 1;
    CHECK(sh_nav_regions_refresh_live(&m, 22, live_valid, live_json, &e) == -1);
    CHECK(m.region_count == 1);

    free(json); free(live1);
}

/* No live surface at all. Both spellings of that leave the map EXACTLY as it
 * was loaded -- a failed refresh falls back to the marks the map arrived with,
 * never to none. */
static void test_live_unreadable(void)
{
    sh_nav_map m, before;
    live_editor e;
    char *json = two_box_map(&m);

    memset(&e, 0, sizeof e);
    e.json[0] = "{}";
    memcpy(&before, &m, sizeof m);

    CHECK(sh_nav_regions_refresh_live(&m, 4, NULL, live_json, &e) == -1);
    CHECK(memcmp(&before, &m, sizeof m) == 0);
    CHECK(sh_nav_regions_refresh_live(&m, 4, live_valid, NULL, &e) == -1);
    CHECK(memcmp(&before, &m, sizeof m) == 0);
    CHECK(sh_nav_regions_refresh_live(&m, 4, live_all_valid, live_all_fail, &e) == -1);
    CHECK(memcmp(&before, &m, sizeof m) == 0);
    CHECK(sh_nav_regions_refresh_live(NULL, 4, live_valid, live_json, &e) == -1);

    /* an editor that answers nothing is the same answer: an empty id range, and
     * a range of ids that are all rejected, are both unread rather than a map
     * whose volumes have all gone */
    CHECK(sh_nav_regions_refresh_live(&m, -1, live_valid, live_json, &e) == -1);
    CHECK(memcmp(&before, &m, sizeof m) == 0);
    memset(&e, 0, sizeof e);
    CHECK(sh_nav_regions_refresh_live(&m, 4, live_valid, live_json, &e) == -1);
    CHECK(memcmp(&before, &m, sizeof m) == 0);

    /* a map this reader did not read has no attribution to offer, so it is
     * refused too rather than attributed from some other map's ownership */
    {
        sh_nav_map other;
        memcpy(&other, &m, sizeof other);
        memset(&e, 0, sizeof e);
        e.json[0] = live_box(TICKED, 0, 0, 0, 128, 128, 64);
        CHECK(sh_nav_regions_refresh_live(&other, 1, live_valid, live_json, &e) == -1);
        CHECK(memcmp(&before, &other, sizeof other) == 0);
        free((void *)e.json[0]);
    }

    free(json);
}

/* More ticked volumes than the region table holds. The cap is reported the same
 * way the load path reports it, and nothing is written past the table. */
static void test_live_region_cap(void)
{
    const int volumes = SH_NAVR_MAX_REGIONS + 8;
    blob inst, ents, kv, vals;
    char edit[1024];
    char *json, *live;
    size_t n;
    sh_nav_map m;
    live_editor e;
    int i;

    bopen(&inst);
    bopen(&ents);
    bopen(&vals);
    bopen(&kv);
    put_instance(&inst, 1, MODULE_DECL, 0, 0, 0, 0);
    /* none of them ticked when the map was read: the whole table comes from the
     * live pass */
    edit_box(edit, sizeof edit, UNTICKED, "", 0, 0, 0, 64, 64, 8);
    for (i = 0; i < volumes; i++) {
        put_entity(&ents, i == 0, i + 1, INHERIT, edit);
        bput(&vals, "%s%d", i ? "," : "", i + 1);
    }
    bput(&kv, "0,%d,%d", volumes, volumes);
    json = map_of(inst.p, ents.p, kv.p, vals.p, &n);

    CHECK(sh_nav_regions_read(json, n, &m) == 1);
    CHECK(m.region_count == 0);
    CHECK(m.truncated == 1); /* Unmarked colliding boxes also consume the solid budget. */

    live = live_box(TICKED, 0, 0, 0, 64, 64, 8);
    memset(&e, 0, sizeof e);
    /* uniqueId i+1, so the live table is addressed one slot up. */
    for (i = 0; i < volumes && i + 1 < LIVE_MAX; i++) e.json[i + 1] = live;

    CHECK(sh_nav_regions_refresh_live(&m, volumes, live_valid, live_json, &e) ==
          SH_NAVR_MAX_REGIONS);
    CHECK(m.region_count == SH_NAVR_MAX_REGIONS);
    CHECK(m.instances[0].region_count == SH_NAVR_MAX_REGIONS);
    CHECK(m.truncated == 1);
    CHECK(m.regions[SH_NAVR_MAX_REGIONS - 1].entity == SH_NAVR_MAX_REGIONS);

    free(json);
    free(live);
    bclose(&inst);
    bclose(&ents);
    bclose(&kv);
    bclose(&vals);
}

/* ==================================================================== */
/* orientation: the quad is the box's real walkable face                 */
/* ==================================================================== */

/* A sparse `spawnOrientation` omits `mat[2]` entirely. Seeded with the IDENTITY
 * that is a rotation; seeded with zeros it is a degenerate matrix and every
 * corner collapses. Over the 6,932 entities carrying a `mat` in a real map, an
 * identity-seeded read yields 6,932 orthonormal matrices with determinant +1
 * and a zero-seeded read yields 1,604 -- so this is the mistake that would
 * quietly break every rotated volume in the game. */
static void test_sparse_orientation_seeds_identity(void)
{
    sh_nav_map m;
    size_t n;
    char *json = map_with_volume(128, 64, 32, 0, 0, 0,
        "\"spawnOrientation\":{\"mat\":{\"mat[0]\":{\"x\":0,\"y\":1},"
        "\"mat[1]\":{\"x\":-1,\"y\":0}}}", &n);

    CHECK(sh_nav_regions_read(json, n, &m) == 1);
    CHECK(m.region_count == 1);
    if (m.region_count == 1) {
        CHECK(near_f(m.regions[0].n[2], 1.0f));
        CHECK(m.regions[0].face == 4);                       /* still the top */
        CHECK(near_f(quad_extent_x(&m.regions[0]), 64.0f));  /* 90 deg yaw swaps */
        CHECK(near_f(quad_extent_y(&m.regions[0]), 128.0f));
        CHECK(near_f(quad_top_z(&m.regions[0]), 32.0f));
    }
    free(json);
}

/* A 90-degree yaw CANNOT pin `R` against `Rt`: for a centred box both give the
 * same corner SET, and at 45 degrees both give equal x and y extents too. Only
 * the actual corner POSITIONS distinguish them. Under Rt (correct) a 200x100
 * box yawed 45 degrees has corners at (+/-35.355, +/-106.066); under R they are
 * at (+/-106.066, +/-35.355). */
static void test_45_yaw_on_oblong_pins_the_convention(void)
{
    sh_nav_map m;
    size_t n;
    char orient[256];
    char *json;
    int i;

    _snprintf_s(orient, sizeof orient, _TRUNCATE,
        "\"spawnOrientation\":{\"mat\":{\"mat[0]\":{\"x\":%.10f,\"y\":%.10f},"
        "\"mat[1]\":{\"x\":%.10f,\"y\":%.10f}}}",
        0.70710678118, 0.70710678118, -0.70710678118, 0.70710678118);
    json = map_with_volume(200, 100, 10, 0, 0, 0, orient, &n);

    CHECK(sh_nav_regions_read(json, n, &m) == 1);
    CHECK(m.region_count == 1);
    if (m.region_count == 1) {
        /* Both conventions give these extents, which is exactly why the extents
         * are not the test. */
        CHECK(near_f(quad_extent_x(&m.regions[0]), 212.132f));
        CHECK(near_f(quad_extent_y(&m.regions[0]), 212.132f));
        for (i = 0; i < 4; i++) {
            float cx = m.regions[0].c[i][0], cy = m.regions[0].c[i][1];
            float ax = cx < 0 ? -cx : cx, ay = cy < 0 ? -cy : cy;
            /* A 200x100 rect at 45 degrees has corners of BOTH shapes -- two at
             * (35.355, 106.066) and two at (106.066, 35.355) -- so the magnitudes
             * alone say nothing. What separates the conventions is the SIGN
             * PAIRING: a +45 yaw puts the long axis along y = x, so every corner
             * has sign(x) == sign(y). Under the transposed reading the yaw is -45
             * and every corner straddles y = -x instead. */
            CHECK((near_f(ax, 35.3553f) && near_f(ay, 106.0660f)) ||
                  (near_f(ax, 106.0660f) && near_f(ay, 35.3553f)));
            CHECK(cx * cy > 0.0f);
        }
    }
    free(json);
}

/* A box on its side -- 58 of the 82 non-upright volumes in one real map sit at
 * exactly 90 degrees. Its walkable face is a SIDE face, footprint size.x by
 * size.z, and the old code put an area a full box-height away from any surface. */
static void test_box_on_its_side_uses_a_side_face(void)
{
    sh_nav_map m;
    size_t n;
    /* 90 degrees about x: local +y -> world +z, local +z -> world -y. */
    char *json = map_with_volume(128, 64, 32, 0, 0, 0,
        "\"spawnOrientation\":{\"mat\":{\"mat[1]\":{\"y\":0,\"z\":1},"
        "\"mat[2]\":{\"y\":-1,\"z\":0}}}", &n);

    CHECK(sh_nav_regions_read(json, n, &m) == 1);
    CHECK(m.region_count == 1);
    if (m.region_count == 1) {
        CHECK(near_f(m.regions[0].n[2], 1.0f));
        CHECK(m.regions[0].face == 2);                       /* +axis1, not the top */
        CHECK(near_f(quad_extent_x(&m.regions[0]), 128.0f));
        CHECK(near_f(quad_extent_y(&m.regions[0]), 32.0f));  /* size.z, not size.y */
    }
    free(json);
}

/* Rotated 180 about x: the box hangs BELOW spawnPosition, so its walkable face
 * is what was the underside, at spawnPosition.z. The old `cz + sz` put the area
 * a box-height up in open air, where nothing can stand and nothing else can
 * claim the column. */
static void test_inverted_box_takes_its_face_from_the_bottom(void)
{
    sh_nav_map m;
    size_t n;
    char *json = map_with_volume(128, 64, 32, 0, 0, 100,
        "\"spawnOrientation\":{\"mat\":{\"mat[1]\":{\"y\":-1},"
        "\"mat[2]\":{\"z\":-1}}}", &n);

    CHECK(sh_nav_regions_read(json, n, &m) == 1);
    CHECK(m.region_count == 1);
    if (m.region_count == 1) {
        CHECK(near_f(quad_top_z(&m.regions[0]), 100.0f));
        CHECK(near_f(m.regions[0].n[2], 1.0f));
        CHECK(m.regions[0].face == 5);                       /* -axis2 */
    }
    free(json);
}

/* An upright volume with no `spawnOrientation` must produce exactly what the
 * rect reader produced -- the regression guard on the 87.6% of volumes in a real
 * map that are upright. */
static void test_absent_orientation_matches_the_old_rect(void)
{
    sh_nav_map m;
    size_t n;
    char *json = map_with_volume(200, 400, 128, 100, 200, 64, NULL, &n);

    CHECK(sh_nav_regions_read(json, n, &m) == 1);
    CHECK(m.region_count == 1);
    if (m.region_count == 1) {
        CHECK(near_f(quad_min_x(&m.regions[0]), 0.0f));
        CHECK(near_f(quad_max_x(&m.regions[0]), 200.0f));
        CHECK(near_f(quad_min_y(&m.regions[0]), 0.0f));
        CHECK(near_f(quad_max_y(&m.regions[0]), 400.0f));
        CHECK(near_f(quad_top_z(&m.regions[0]), 192.0f));    /* bottom 64 + 128 */
        CHECK(m.regions[0].face == 4);
    }
    free(json);
}

/* Not a rotation at all: refused rather than emitted sheared. */
static void test_non_orthonormal_matrix_is_refused(void)
{
    sh_nav_map m;
    size_t n;
    char *json = map_with_volume(128, 64, 32, 0, 0, 0,
        "\"spawnOrientation\":{\"mat\":{\"mat[0]\":{\"x\":3,\"y\":0,\"z\":0}}}", &n);

    CHECK(sh_nav_regions_read(json, n, &m) == 1);
    CHECK(m.region_count == 0);
    CHECK(m.invalid_geometry == 1);
    free(json);
}

/* A REFLECTION is orthonormal but has determinant -1, and the shoelace rewind
 * would quietly make its mirrored footprint look legal. */
static void test_reflection_is_refused(void)
{
    sh_nav_map m;
    size_t n;
    char *json = map_with_volume(128, 64, 32, 0, 0, 0,
        "\"spawnOrientation\":{\"mat\":{\"mat[0]\":{\"x\":-1}}}", &n);

    CHECK(sh_nav_regions_read(json, n, &m) == 1);
    CHECK(m.region_count == 0);
    CHECK(m.invalid_geometry == 1);
    free(json);
}

int main(void)
{
    test_one_volume();
    test_sparse_orientation_seeds_identity();
    test_45_yaw_on_oblong_pins_the_convention();
    test_box_on_its_side_uses_a_side_face();
    test_inverted_box_takes_its_face_from_the_bottom();
    test_absent_orientation_matches_the_old_rect();
    test_non_orthonormal_matrix_is_refused();
    test_reflection_is_refused();
    test_marker_required();
    test_box_shape();
    test_two_instances_of_one_module();
    test_orphan_bucket();
    test_unnameable_instance();
    test_truncation();
    test_malformed();
    test_live_tick_this_session();
    test_live_untick_this_session();
    test_live_no_volumes_keeps_map_marks();
    test_live_marked_but_unattributable_keeps_map_marks();
    test_live_unmarked_volumes_still_clear();
    test_live_absent_marker_is_false();
    test_live_block_demons();
    test_live_volume_without_attribution();
    test_live_valid_gates_the_scan();
    test_live_unreadable();
    test_live_region_cap();

    if (g_failed) {
        fprintf(stderr, "%d check(s) FAILED\n", g_failed);
        return 1;
    }
    printf("nav_regions_test: all checks passed\n");
    return 0;
}
