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
        CHECK(near_f(m.regions[0].x0, 0.0f));
        CHECK(near_f(m.regions[0].x1, 200.0f));
        CHECK(near_f(m.regions[0].y0, 0.0f));
        CHECK(near_f(m.regions[0].y1, 400.0f));
        CHECK(near_f(m.regions[0].top_z, 192.0f));   /* bottom 64 + height 128 */
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
        CHECK(near_f(m.regions[1].x0, -256.0f));
        CHECK(near_f(m.regions[1].x1, 256.0f));
        CHECK(near_f(m.regions[1].y0, -128.0f));
        CHECK(near_f(m.regions[1].y1, 128.0f));
        CHECK(near_f(m.regions[1].top_z, 48.0f));
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
        CHECK(near_f(m.regions[1].x0, -64.0f) && near_f(m.regions[1].x1, 64.0f));
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

    /* a multimap that stops short: two instances, but keyValues only bounds the
     * first bucket. The second bucket has no end, so it is not a bucket, and
     * inventing one out of whatever follows would attribute a volume to an
     * instance the map never said owned it. */
    json = map_of(inst.p, ents.p, "0,1", "11,22", &n);
    CHECK(sh_nav_regions_read(json, n, &m) == 1);
    CHECK(m.region_count == 1);
    CHECK(m.instances[0].region_count == 1);
    CHECK(m.instances[1].region_count == 0);
    if (m.region_count == 1) CHECK(m.regions[0].instance == 0);
    free(json);

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

int main(void)
{
    test_one_volume();
    test_marker_required();
    test_box_shape();
    test_two_instances_of_one_module();
    test_orphan_bucket();
    test_unnameable_instance();
    test_truncation();
    test_malformed();

    if (g_failed) {
        fprintf(stderr, "%d check(s) FAILED\n", g_failed);
        return 1;
    }
    printf("nav_regions_test: all checks passed\n");
    return 0;
}
