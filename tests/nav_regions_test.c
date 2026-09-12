/* Tests navigation-volume extraction and instance ownership from synthetic
 * maps. Identical module-local coordinates in separate instances must remain
 * distinct, and malformed documents must return an empty result. */
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nav_regions.h"
#include "overrides_baked.h"

void backend_log(const char *message) { (void)message; }

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

/* Quad extrema preserve the upright-volume expectations below. */
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

/* The decoy renderModelInfo.size must not replace clipModelInfo.size. */
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
/* Build one attributed, marked Blocking Box. orient supplies optional raw
 * spawnOrientation JSON. The caller owns the completed document. */
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
    edit_box_or(edit, sizeof edit, "\"flags\":{\"noFlood\":true},\"blockDemons\":true,", "",
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
    edit_box(edit, sizeof edit, "\"flags\":{\"noFlood\":true},\"blockDemons\":true,", "",
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

/* Only `flags.noFlood` makes a region. A map full of ordinary blocking
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
    edit_box(edit, sizeof edit, "\"flags\":{\"noFlood\":false},\"blockDemons\":true,", "",
             0, 0, 0, 64, 64, 8);
    put_entity(&ents, 0, 2, INHERIT, edit);
    edit_box(edit, sizeof edit, "\"flags\":{\"noFlood\":true},", "", 0, 0, 0, 64, 64, 8);
    put_entity(&ents, 0, 3, INHERIT, edit);
    /* and an entity that is not a blocking volume at all, ticked or not */
    edit_box(edit, sizeof edit, "\"flags\":{\"noFlood\":true},", "", 0, 0, 0, 64, 64, 8);
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
    static const char *TICKED = "\"flags\":{\"noFlood\":true},\"blockDemons\":true,";
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
               "\"flags\":{\"noFlood\":true},\"blockDemons\":true,"
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

/* Identical local geometry in two module instances must remain distinct.
 * Match instance order to the instanceEntities buckets. */
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
    edit_box(edit, sizeof edit, "\"flags\":{\"noFlood\":true},\"blockDemons\":true,", "",
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

/* Skip orphan volumes; module-local coordinates cannot identify an owner. */
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
    edit_box(edit, sizeof edit, "\"flags\":{\"noFlood\":true},\"blockDemons\":true,", "",
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

/* An invalid module name keeps its instance slot but contributes no volumes. */
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
    edit_box(edit, sizeof edit, "\"flags\":{\"noFlood\":true},\"blockDemons\":true,", "",
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
    edit_box(edit, sizeof edit, "\"flags\":{\"noFlood\":true},\"blockDemons\":true,", "",
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

/* A refused document must overwrite out with an empty result. */
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
    edit_box(edit, sizeof edit, "\"flags\":{\"noFlood\":true},\"blockDemons\":true,", "",
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

/* Simulate unsaved editor state with synthetic entity documents. The live
 * table is sparse and indexed by uniqueId, not the map JSON array position.
 * Documents are placed at IDs 11 and 22 and may omit their own uniqueId member;
 * the callback address supplies identity. */

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
    /* Poison bytes after the reported length to catch reads beyond the document. */
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

static const char *TICKED  = "\"flags\":{\"noFlood\":true},\"blockDemons\":true,";
static const char *UNTICKED = "\"blockDemons\":true,";

/* Deliberately unrelated to the shipped declaration's dimensions. */
static int resolved_size(unsigned entity, float size[3], void *ctx)
{
    CHECK(entity == 0);
    if (!ctx) return 0;
    memcpy(size, ctx, 3 * sizeof(float)); return 1;
}

static void test_resolved_decl_size(void)
{
    static const char good[] =
        "edit = { renderModelInfo = { scale = { x=999;y=999;z=999; } }"
        " note = \"clipModelInfo = { size={x=1;y=1;z=1;} }\";"
        " /* clipModelInfo = {} */ clipModelInfo = { type=\"CLIPMODEL_BOX\";"
        "size={z=9.125e1; x=7.35e1; // unrelated y=1\n y=157.25;} } }";
    static const char *bad[] = {
        "edit={clipModelInfo={type=\"CLIPMODEL_BOX\";size={x=1;y=2;}}}",
        "edit={clipModelInfo={type=\"CLIPMODEL_BOX\";size={x=0;y=2;z=3;}}}",
        "edit={clipModelInfo={type=\"CLIPMODEL_BOX\";size={x=-1;y=2;z=3;}}}",
        "edit={clipModelInfo={type=\"CLIPMODEL_BOX\";size={x=1e999;y=2;z=3;}}}",
        "edit={clipModelInfo={type=\"CLIPMODEL_BOX\";size={x=1foo;y=2;z=3;}}}",
        "edit={clipModelInfo={type=\"CLIPMODEL_BOX\";size={x=\"1\";y=2;z=3;}}}",
        "edit={clipModelInfo={type=\"CLIPMODEL_CYLINDER\";size={x=1;y=2;z=3;}}}",
        "edit={clipModelInfo={size={x=1;y=2;z=3;}}}",
        "edit={renderModelInfo={scale={x=1;y=2;z=3;}}}",
        "edit={clipModelInfo={type=\"CLIPMODEL_BOX\";size={x=1;y=2;z=3;}}",
        "edit={/* unterminated"
    };
    float size[3];
    size_t i;
    CHECK(sh_nav_regions_decl_size(good, sizeof good-1, size));
    CHECK(near_f(size[0],73.5f) && near_f(size[1],157.25f) && near_f(size[2],91.25f));
    for (i=0;i<sizeof bad/sizeof bad[0];i++)
        CHECK(!sh_nav_regions_decl_size(bad[i],strlen(bad[i]),size));
    for (i=0;i<sizeof good-2;i++) CHECK(!sh_nav_regions_decl_size(good,i,size));
}

static void test_inherited_box_dimensions(void)
{
    static const struct {
        const char *geometry;
        float x, y, z;
        int valid;
    } cases[] = {
        { "", 73, 157, 91, 1 },
        { "\"clipModelInfo\":{}", 73, 157, 91, 1 },
        { "\"clipModelInfo\":{\"size\":{}}", 73, 157, 91, 1 },
        { "\"clipModelInfo\":{\"size\":{\"x\":132}}", 132, 157, 91, 1 },
        { "\"clipModelInfo\":{\"size\":{\"y\":192}}", 73, 192, 91, 1 },
        { "\"clipModelInfo\":{\"size\":{\"z\":96}}", 73, 157, 96, 1 },
        { "\"clipModelInfo\":{\"size\":{\"x\":132,\"y\":192}}", 132, 192, 91, 1 },
        { "\"clipModelInfo\":{\"size\":{\"x\":132,\"z\":96}}", 132, 157, 96, 1 },
        { "\"clipModelInfo\":{\"size\":{\"y\":192,\"z\":96}}", 73, 192, 96, 1 },
        { "\"clipModelInfo\":{\"size\":{\"x\":132,\"y\":192,\"z\":96}}", 132, 192, 96, 1 },
        { "\"clipModelInfo\":{\"size\":{\"x\":0}}", 0, 0, 0, 0 },
        { "\"clipModelInfo\":{\"size\":{\"y\":-1}}", 0, 0, 0, 0 },
        { "\"clipModelInfo\":{\"size\":{\"z\":0}}", 0, 0, 0, 0 },
        { "\"clipModelInfo\":{\"size\":{\"z\":1e999}}", 0, 0, 0, 0 },
        { "\"clipModelInfo\":{\"size\":{\"x\":null}}", 0, 0, 0, 0 },
        { "\"clipModelInfo\":{\"size\":{\"y\":\"128\"}}", 0, 0, 0, 0 },
        { "\"clipModelInfo\":null", 0, 0, 0, 0 },
        { "\"clipModelInfo\":[]", 0, 0, 0, 0 },
        { "\"clipModelInfo\":{\"size\":null}", 0, 0, 0, 0 },
        { "\"clipModelInfo\":{\"size\":[]}", 0, 0, 0, 0 },
        { "\"clipModelInfo\":{\"type\":null}", 0, 0, 0, 0 },
        { "\"clipModelInfo\":{\"type\":\"CLIPMODEL_CYLINDER\"}", 0, 0, 0, 0 }
    };
    size_t c;
    int marked;
    for (c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        for (marked = 0; marked <= 1; marked++) {
            blob inst, ents, edit;
            char *json;
            size_t n;
            sh_nav_map m;
            bopen(&inst); bopen(&ents); bopen(&edit);
            put_instance(&inst, 1, MODULE_DECL, 0, 0, 0, 0);
            bput(&edit, "%s\"spawnPosition\":{\"z\":32},"
                        "\"renderModelInfo\":{\"scale\":{\"x\":999,\"y\":999,\"z\":999}}%s%s",
                 marked ? TICKED : UNTICKED,
                 cases[c].geometry[0] ? "," : "", cases[c].geometry);
            put_entity(&ents, 1, 7, INHERIT, edit.p);
            json = map_of(inst.p, ents.p, "0,1,1", "7", &n);
            float effective[3] = {cases[c].x, cases[c].y, cases[c].z};
            /* Explicit malformed JSON must still fail even with a valid native size. */
            if (!cases[c].valid) { effective[0]=73; effective[1]=157; effective[2]=91; }
            CHECK(sh_nav_regions_read_resolved(json, n, &m, resolved_size, effective));
            CHECK(m.invalid_geometry == !cases[c].valid);
            CHECK(!m.truncated);
            CHECK(m.region_count == (marked && cases[c].valid));
            CHECK(m.obstacle_count == (!marked && cases[c].valid));
            if (cases[c].valid) {
                sh_nav_region *r = marked ? &m.regions[0] : &m.obstacles[0];
                CHECK(near_f(quad_extent_x(r), cases[c].x));
                CHECK(near_f(quad_extent_y(r), cases[c].y));
                CHECK(near_f(quad_top_z(r), 32 + cases[c].z));
                CHECK(r->instance == 0);
                /* A failed current entity read cannot reuse an earlier size. */
                CHECK(sh_nav_regions_read_resolved(json, n, &m, resolved_size, NULL));
                CHECK(m.invalid_geometry);
            }
            free(json); bclose(&inst); bclose(&ents); bclose(&edit);
        }
    }
}

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

/* An unsaved AI Navigation toggle must update the volume while retaining
 * the instance ownership recorded at load. */
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
    char *live0 = live_box("\"flags\":{\"noFlood\":false},\"blockDemons\":true,", 0, 0, 0, 128, 128, 64);
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

/* A playtest surface may contain entities but no Blocking Boxes. Preserve
 * loaded marks until the surface can distinguish an unticked box from an
 * unavailable editor volume. */
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

/* Preserve loaded marks when live IDs cannot be attributed to their recorded
 * owners. A different ID space is not evidence that the map has no marks. */
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

/* A surface that still exposes the Blocking Boxes may explicitly unmark them. */
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

/* Missing flags.noFlood means unmarked, matching the serialized-map default. */
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

/* Preserve blockDemons, including its absent=false default, for floor admission. */
static void test_live_block_demons(void)
{
    sh_nav_map m;
    live_editor e;
    char *json = two_box_map(&m);
    char *live0 = live_box("\"flags\":{\"noFlood\":true},", 0, 0, 0, 128, 128, 64);
    char *live1 = live_box("\"flags\":{\"noFlood\":true},\"blockDemons\":true,",
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

/* Skip newly observed volumes without recorded owners; report the lost attribution. */
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

    /* An ID recorded for a non-volume does not establish volume ownership. */
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

    /* Reject a serializer length beyond the supplied buffer. With no legible
     * Blocking Box left, preserve the loaded marks. */
    json = two_box_map(&m);
    memset(&e, 0, sizeof e);
    e.json[22] = live1;
    e.overrun[22] = 1;
    CHECK(sh_nav_regions_refresh_live(&m, 22, live_valid, live_json, &e) == -1);
    CHECK(m.region_count == 1);

    free(json); free(live1);
}

/* Without a live surface, retain the marks captured at load. */
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

    /* An empty or wholly unreadable ID range also preserves loaded marks. */
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

/* Sparse spawnOrientation matrices inherit missing rows from identity.
 * Zero defaults would collapse corners when mat[2] is omitted. */
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

/* Check corner positions to distinguish R from its transpose; symmetric
 * extents alone cannot reveal reversed yaw. */
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
            /* Check sign pairing as well as magnitudes: +45-degree yaw aligns the
             * long axis with y=x; the transposed interpretation aligns it with y=-x. */
            CHECK((near_f(ax, 35.3553f) && near_f(ay, 106.0660f)) ||
                  (near_f(ax, 106.0660f) && near_f(ay, 35.3553f)));
            CHECK(cx * cy > 0.0f);
        }
    }
    free(json);
}

/* At a 90-degree tilt the walkable surface is a side face with size.x by size.z. */
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

/* A 180-degree x rotation puts the box below spawnPosition; its walkable
 * underside lies at spawnPosition.z. */
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

/* Omitted spawnOrientation must preserve the upright-volume result. */
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

static void test_marker_migration(void)
{
    static const char *cases[] = {
        "\"affectsNavmesh\":true,",
        "\"affectsNavmesh\":true,\"flags\":{},",
        "\"flags\":{\"hide\":false},\"affectsNavmesh\":true,",
        "\"flags\":{\"noFlood\":true,\"hide\":false},\"affectsNavmesh\":true,",
        "\"flags\":{\"noFlood\":false,\"hide\":false},\"affectsNavmesh\":true,"
    };
    size_t k;
    for (k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        blob inst, ents;
        char edit[1024], *json, *migrated;
        size_t len, out_len;
        sh_nav_map before, after;
        bopen(&inst); bopen(&ents);
        put_instance(&inst, 1, MODULE_DECL, 0, 0, 0, 0);
        edit_box(edit, sizeof edit, cases[k], "", 0, 0, 0, 64, 64, 8);
        put_entity(&ents, 1, 1, INHERIT, edit);
        json = map_of(inst.p, ents.p, "0,1,1", "1", &len);
        CHECK(sh_nav_regions_read(json, len, &before));
        /* Legacy field alone never participates in a bake. */
        CHECK(before.region_count == (k == 3 ? 1 : 0));
        migrated = sh_nav_regions_migrate(json, len, &out_len);
        CHECK(migrated != NULL);
        if (migrated) {
            CHECK(out_len == strlen(migrated));
            CHECK(strstr(migrated, "\"affectsNavmesh\":false") != NULL);
            CHECK(sh_nav_regions_read(migrated, out_len, &after));
            CHECK(after.region_count == (k == 4 ? 0 : 1));
            if (after.region_count) CHECK(after.regions[0].instance == 0);
            if (k >= 2) CHECK(strstr(migrated, "\"hide\":false") != NULL);
            CHECK(sh_nav_regions_migrate(migrated, out_len, NULL) == NULL);
            HeapFree(GetProcessHeap(), 0, migrated);
        }
        /* Input ownership and old file bytes remain untouched. */
        CHECK(strstr(json, "\"affectsNavmesh\":true") != NULL);
        free(json); bclose(&inst); bclose(&ents);
    }
}

static void test_marker_migration_boundaries(void)
{
    static const char *unchanged[] = {
        "{\"entities\":[]}",
        "{\"entities\":[{\"entityDef\":{\"inherit\":\"other\",\"state\":{\"edit\":{\"affectsNavmesh\":true}}}}]}",
        "{\"entities\":[{\"entityDef\":{\"inherit\":\"snapmaps/volume/blocking\",\"state\":{\"edit\":{\"affectsNavmesh\":false}}}}]}",
        "{\"entities\":[{\"entityDef\":{\"inherit\":\"snapmaps/volume/blocking\",\"state\":{\"edit\":{\"nested\":{\"affectsNavmesh\":true}}}}}]}",
        "{\"entities\":[{\"entityDef\":{\"inherit\":\"snapmaps/volume/blocking\",\"state\":{\"edit\":{\"affectsNavmesh\":true,\"flags\":null}}}}]}",
        "{\"entities\":[{\"entityDef\":{\"inherit\":\"snapmaps/volume/blocking\",\"state\":{\"edit\":{\"affectsNavmesh\":true,\"flags\":{\"noFlood\":\"false\"}}}}}]}",
        "{\"entities\":[{\"entityDef\":{\"inherit\":\"snapmaps/volume/blocking\",\"state\":{\"edit\":{\"affectsNavmesh\":trueSuffix}}}}]}",
        "{\"entities\":["
    };
    size_t i;
    for (i = 0; i < sizeof unchanged / sizeof unchanged[0]; i++) {
        size_t len = strlen(unchanged[i]), out_len = 0;
        CHECK(sh_nav_regions_migrate(unchanged[i], len, &out_len) == NULL);
        CHECK(out_len == len);
    }
    /* The actual shipped native property points to the new field. Its native
     * affectsNavmeshPath stays independent and no new engine field is added. */
    {
        char text[sizeof g_ov_baked_d4 + 1];
        memcpy(text, g_ov_baked_d4, sizeof g_ov_baked_d4);
        text[sizeof g_ov_baked_d4] = 0;
        CHECK(strstr(text, "path = \"flags.noFlood\";") != NULL);
        CHECK(strstr(text, "path = \"affectsNavmesh\";") == NULL);
        CHECK(strstr(text, "affectsNavmeshPath = \"affectsNavmesh\";") != NULL);
        CHECK(strstr(text, "#str_sh_bv_navigation") != NULL);
    }
}

static void test_duplicate_box_ids(void)
{
    blob inst, ents;
    char edit[1024], *json;
    size_t n;
    sh_nav_map m;
    int repeated;

    bopen(&inst); bopen(&ents);
    put_instance(&inst, 1, MODULE_DECL, 0, 0, 0, 0);
    edit_box(edit, sizeof edit, "\"flags\":{\"noFlood\":true},\"blockDemons\":true,", "",
             0, 0, 0, 64, 64, 8);
    put_entity(&ents, 1, 7, INHERIT, edit);
    edit_box(edit, sizeof edit, "\"flags\":{\"noFlood\":true},\"blockDemons\":true,", "",
             256, 0, 0, 64, 64, 8);
    put_entity(&ents, 0, 7, INHERIT, edit);
    /* Either one reference or repeated references name the same owning room. */
    for (repeated = 0; repeated < 2; repeated++) {
        live_editor live = {0};
        int reads = -1;
        const char *why = NULL;
        json = map_of(inst.p, ents.p, repeated ? "0,2,2" : "0,1,1",
                      repeated ? "7,7" : "7", &n);
        CHECK(sh_nav_regions_read(json, n, &m));
        CHECK(m.ids_unusable);
        CHECK(!m.invalid_geometry);
        CHECK(m.region_count == 2);
        CHECK(m.instances[0].region_count == 2);
        CHECK(m.regions[0].instance == 0 && m.regions[1].instance == 0);
        CHECK(near_f(quad_min_x(&m.regions[1]) - quad_min_x(&m.regions[0]), 256));
        CHECK(!sh_nav_regions_refresh_known(&m, live_valid, live_json, &live,
                                            &reads, &why));
        CHECK(reads == 0 && live.valid_calls == 0);
        free(json);
    }
    /* The shared ID cannot decide which box belongs to which room. */
    put_instance(&inst, 0, MODULE_DECL, 1024, 0, 0, 0);
    json = map_of(inst.p, ents.p, "0,1,2,2", "7,7", &n);
    CHECK(sh_nav_regions_read(json, n, &m));
    CHECK(m.ids_unusable && m.invalid_geometry);
    free(json);
    bclose(&inst); bclose(&ents);

    /* A duplicate with no navigation flags must also disable lookup by ID. */
    bopen(&inst); bopen(&ents);
    put_instance(&inst, 1, MODULE_DECL, 0, 0, 0, 0);
    put_entity(&ents, 1, 7, INHERIT, edit);
    edit_box(edit, sizeof edit, "", "", 512, 0, 0, 64, 64, 8);
    put_entity(&ents, 0, 7, INHERIT, edit);
    json = map_of(inst.p, ents.p, "0,1,1", "7", &n);
    CHECK(sh_nav_regions_read(json, n, &m));
    CHECK(m.ids_unusable && !m.invalid_geometry);
    CHECK(m.region_count == 1);
    {
        live_editor live = {0};
        CHECK(!sh_nav_regions_refresh_known(&m, live_valid, live_json, &live,
                                            NULL, NULL));
        CHECK(live.valid_calls == 0);
    }
    free(json); bclose(&inst); bclose(&ents);
}

int main(void)
{
    test_resolved_decl_size();
    test_inherited_box_dimensions();
    test_duplicate_box_ids();
    test_marker_migration();
    test_marker_migration_boundaries();
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
