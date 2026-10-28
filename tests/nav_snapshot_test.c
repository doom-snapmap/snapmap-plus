/* Tests which map object the Play navigation snapshot reads.
 *
 * A conversion reads the map being converted. The editor's map stands in only
 * when it is that object, or when the converted map is unreadable and the
 * editor's map places exactly the converted modules -- never an earlier
 * session's different map. An omitted Blocking Box axis inherits the
 * entityDef size (nav_entitydef) during a conversion. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "nav_snapshot.h"
#include "nav_regions.h"
#include "nav_entitydef.h"

void backend_log(const char *message) { (void)message; }

static int g_failed;

#define CHECK(expr) do {                                                         \
    if (!(expr)) {                                                               \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++;                                                              \
    }                                                                            \
} while (0)

/* Three fake idSnapMapEdit objects. */
static const int BUILD = 1, EDIT = 2, STALE = 3;

typedef struct fake_world {
    int build_readable, edit_readable, stale_readable;
    const char *build_modules[4];   /* what the conversion's records name */
    int build_module_count;         /* -1: the records cannot be read */
    int reads, edit_reads, build_reads;
    int converting_seen[4];         /* per object */
} fake_world;

/* The map tables are heap-backed: a filled map owns its room table. */
static void fill(sh_nav_map *out, const char *const *modules, int count, int regions)
{
    int i;
    memset(out, 0, sizeof *out);
    out->instances = (sh_nav_instance *)calloc(count ? count : 1, sizeof *out->instances);
    if (!out->instances) abort();
    out->instance_count = out->instance_cap = count;
    for (i = 0; i < count; i++)
        strcpy_s(out->instances[i].module, sizeof out->instances[i].module, modules[i]);
    out->region_count = regions;
}

static int fake_read(const void *object, int converting, sh_nav_map *out, void *ctx)
{
    fake_world *w = (fake_world *)ctx;
    static const char *this_map[] = { "ind_dlc/room_a", "ind_dlc/room_b" };
    static const char *other_map[] = { "ind_dlc/room_c" };
    int id = *(const int *)object;
    w->reads++;
    w->converting_seen[id] = converting;
    /* A read overwrites `out` without freeing it (nav_regions.h); the caller
     * must have released what it held, or the tables leak. A refused read
     * leaves an empty map, as sh_nav_regions_read_resolved does. */
    memset(out, 0, sizeof *out);
    if (id == BUILD) {
        w->build_reads++;
        if (!w->build_readable) return 0;
        fill(out, this_map, 2, 3);
        return 1;
    }
    if (id == EDIT) {
        w->edit_reads++;
        if (!w->edit_readable) return 0;
        fill(out, this_map, 2, 5);
        return 1;
    }
    if (!w->stale_readable) return 0;
    fill(out, other_map, 1, 7);
    return 1;
}

static int fake_modules(const void *object, char *names, size_t name_cap, int cap, void *ctx)
{
    fake_world *w = (fake_world *)ctx;
    int i;
    (void)object;
    if (w->build_module_count < 0 || w->build_module_count > cap) return -1;
    for (i = 0; i < w->build_module_count; i++)
        strcpy_s(names + (size_t)i * name_cap, name_cap, w->build_modules[i]);
    return w->build_module_count;
}

static fake_world world(void)
{
    fake_world w;
    memset(&w, 0, sizeof w);
    w.build_readable = w.edit_readable = w.stale_readable = 1;
    w.build_modules[0] = "maps/modules/ind_dlc/room_a";
    w.build_modules[1] = "maps/modules/ind_dlc/room_b";
    w.build_module_count = 2;
    return w;
}

static sh_nav_map g_map;

static int take(fake_world *w, const int *build, const int *edit, sh_nav_snapshot_result *r)
{
    sh_nav_snapshot_sources src;
    memset(&src, 0, sizeof src);
    src.build = build; src.edit = edit;
    src.read = fake_read; src.modules = fake_modules; src.ctx = w;
    sh_nav_map_free(&g_map);
    return sh_nav_snapshot_take(&src, &g_map, r);
}

/* Editor preview and Build Navmesh: no conversion, the edit map as before. */
static void test_no_conversion_reads_the_editor(void)
{
    fake_world w = world();
    sh_nav_snapshot_result r;
    CHECK(take(&w, NULL, &EDIT, &r));
    CHECK(r.source == SH_NAV_SNAPSHOT_EDITOR);
    CHECK(g_map.region_count == 5);
    CHECK(w.converting_seen[EDIT] == 0);    /* an unmeasured box still waits */
    CHECK(w.build_reads == 0);

    w = world();
    CHECK(!take(&w, NULL, NULL, &r));
    CHECK(r.source == SH_NAV_SNAPSHOT_NONE);
    CHECK(strstr(r.why, "no editor map") != NULL);
    CHECK(w.reads == 0);

    w = world(); w.edit_readable = 0;
    CHECK(!take(&w, NULL, &EDIT, &r));
    CHECK(r.source == SH_NAV_SNAPSHOT_NONE);
}

/* Hub, map browser, co-op or direct launch in a fresh session: no editor map
 * exists, and the converted map is all there is. */
static void test_launch_without_editor_reads_the_conversion(void)
{
    fake_world w = world();
    sh_nav_snapshot_result r;
    CHECK(take(&w, &BUILD, NULL, &r));
    CHECK(r.source == SH_NAV_SNAPSHOT_CONVERTED);
    CHECK(g_map.region_count == 3 && g_map.instance_count == 2);
    CHECK(w.converting_seen[BUILD] == 1);
    CHECK(w.reads == 1);
    CHECK(r.why[0] == 0);
}

/* The editor's map is still set from an earlier session. The conversion wins
 * and the old map is never even read. */
static void test_stale_editor_map_is_not_read(void)
{
    fake_world w = world();
    sh_nav_snapshot_result r;
    CHECK(take(&w, &BUILD, &STALE, &r));
    CHECK(r.source == SH_NAV_SNAPSHOT_CONVERTED);
    CHECK(g_map.region_count == 3);
    CHECK(w.reads == 1 && w.build_reads == 1);
}

/* Editor Play: the conversion is a copy of the editor's map, read once. */
static void test_editor_play_reads_the_conversion_once(void)
{
    fake_world w = world();
    sh_nav_snapshot_result r;
    CHECK(take(&w, &BUILD, &EDIT, &r));
    CHECK(r.source == SH_NAV_SNAPSHOT_CONVERTED);
    CHECK(w.reads == 1 && w.edit_reads == 0);
}

/* When the converted object is the editor's own map, one read of it. */
static void test_same_object(void)
{
    fake_world w = world();
    sh_nav_snapshot_result r;
    CHECK(take(&w, &BUILD, &BUILD, &r));
    CHECK(r.source == SH_NAV_SNAPSHOT_EDITOR_SAME);
    CHECK(w.reads == 1);

    /* Refused: no second read of the same object. */
    w = world(); w.build_readable = 0;
    CHECK(!take(&w, &BUILD, &BUILD, &r));
    CHECK(r.source == SH_NAV_SNAPSHOT_NONE);
    CHECK(w.reads == 1);
}

/* The conversion cannot be read; the editor's map places the same modules. */
static void test_fallback_to_matching_editor_map(void)
{
    fake_world w = world();
    sh_nav_snapshot_result r;
    w.build_readable = 0;
    CHECK(take(&w, &BUILD, &EDIT, &r));
    CHECK(r.source == SH_NAV_SNAPSHOT_EDITOR_FALLBACK);
    CHECK(g_map.region_count == 5);
    CHECK(w.converting_seen[EDIT] == 1);
    CHECK(strstr(r.why, "could not be read") != NULL);
}

/* The conversion cannot be read and the editor holds another map: nothing,
 * and no stale geometry survives in the output. */
static void test_no_fallback_to_a_different_map(void)
{
    fake_world w = world();
    sh_nav_snapshot_result r;
    int i, clean = 1;
    w.build_readable = 0;
    CHECK(!take(&w, &BUILD, &STALE, &r));
    CHECK(r.source == SH_NAV_SNAPSHOT_NONE);
    CHECK(strstr(r.why, "different map") != NULL);
    for (i = 0; i < (int)sizeof g_map; i++) if (((unsigned char *)&g_map)[i]) clean = 0;
    CHECK(clean);

    /* Neither readable. */
    w = world(); w.build_readable = 0; w.stale_readable = 0;
    CHECK(!take(&w, &BUILD, &STALE, &r));
    CHECK(strstr(r.why, "nor could the editor") != NULL);

    /* No editor map at all. */
    w = world(); w.build_readable = 0;
    CHECK(!take(&w, &BUILD, NULL, &r));
    CHECK(r.source == SH_NAV_SNAPSHOT_NONE);
    CHECK(w.reads == 1);
}

/* The converted read must describe the conversion's own records. */
static void test_conversion_must_match_its_records(void)
{
    fake_world w = world();
    sh_nav_snapshot_result r;
    w.build_modules[1] = "maps/modules/ind_dlc/room_z";
    /* The editor's map names a then b too, so it does not match either. */
    CHECK(!take(&w, &BUILD, &EDIT, &r));
    CHECK(r.source == SH_NAV_SNAPSHOT_NONE);
    CHECK(strstr(r.why, "copy 1") != NULL);

    w = world(); w.build_module_count = 3; w.build_modules[2] = "maps/modules/x/y";
    CHECK(!take(&w, &BUILD, NULL, &r));
    CHECK(strstr(r.why, "places 2 module(s) and the conversion places 3") != NULL);

    w = world(); w.build_module_count = -1;
    CHECK(!take(&w, &BUILD, NULL, &r));
    CHECK(strstr(r.why, "modules could not be read") != NULL);

    /* Without a module lister nothing can be checked, so nothing is used. */
    {
        sh_nav_snapshot_sources src;
        w = world();
        memset(&src, 0, sizeof src);
        src.build = &BUILD; src.read = fake_read; src.ctx = &w;
        CHECK(!sh_nav_snapshot_take(&src, &g_map, &r));
    }
}

static void test_module_key(void)
{
    char key[SH_NAVR_MODULE_CAP];
    CHECK(sh_nav_snapshot_module_key("maps/modules/ind_dlc/room_a", key, sizeof key));
    CHECK(!strcmp(key, "ind_dlc/room_a"));
    CHECK(sh_nav_snapshot_module_key("maps/modules/ind_dlc/room_a.decl", key, sizeof key));
    CHECK(!strcmp(key, "ind_dlc/room_a"));
    CHECK(sh_nav_snapshot_module_key("maps/modules/smpgrid/v1/c/4_4_2", key, sizeof key));
    CHECK(!strcmp(key, "smpgrid/v1/c/4_4_2"));
    CHECK(!sh_nav_snapshot_module_key("maps/other/room_a", key, sizeof key));
    CHECK(!sh_nav_snapshot_module_key("maps/modules/", key, sizeof key));
    CHECK(!sh_nav_snapshot_module_key(NULL, key, sizeof key));
    CHECK(!sh_nav_snapshot_module_key("maps/modules/a/b", key, 3));
}

static void test_modules_match(void)
{
    char names[2][SH_NAVR_MODULE_CAP] = { "maps/modules/a/b", "maps/modules/c/d.decl" };
    char why[160];
    static const char *mods[] = { "a/b", "c/d" };
    sh_nav_map m;
    fill(&m, mods, 2, 0);
    CHECK(sh_nav_snapshot_modules_match(&m, names[0], SH_NAVR_MODULE_CAP, 2, why, sizeof why));
    CHECK(!sh_nav_snapshot_modules_match(&m, names[0], SH_NAVR_MODULE_CAP, 1, why, sizeof why));
    /* An instance the reader could not name never matches, even an empty one. */
    m.instances[1].module[0] = 0;
    names[1][0] = 0;
    CHECK(!sh_nav_snapshot_modules_match(&m, names[0], SH_NAVR_MODULE_CAP, 2, why, sizeof why));
    CHECK(strstr(why, "copy 1") != NULL);
    /* An empty map matches an empty conversion. */
    sh_nav_map_free(&m);
    CHECK(sh_nav_snapshot_modules_match(&m, NULL, 0, 0, NULL, 0));
}

/* ---- inherited size through the real reader ---------------------------- */

static int inherited_only(unsigned entity, float size[3], void *ctx)
{
    (void)entity; (void)ctx;
    return sh_nav_entitydef_box_size("snapmaps/volume/blocking", size);
}

static int unresolved(unsigned entity, float size[3], void *ctx)
{
    (void)entity; (void)size; (void)ctx;
    return 0;
}

static float extent(const sh_nav_region *r, int axis)
{
    float lo = r->c[0][axis], hi = r->c[0][axis];
    int i;
    for (i = 1; i < 4; i++) {
        if (r->c[i][axis] < lo) lo = r->c[i][axis];
        if (r->c[i][axis] > hi) hi = r->c[i][axis];
    }
    return hi - lo;
}

/* The conversion-time size comes from the shared resolver (nav_entitydef),
 * the one the map-text reads at load and after Save use, without a reader. */
static void test_inherited_size(void)
{
    float s[3] = {0, 0, 0};
    CHECK(sh_nav_entitydef_box_size("snapmaps/volume/blocking", s));
    CHECK(s[0] == 16.0f && s[1] == 128.0f && s[2] == 64.0f);
    CHECK(!sh_nav_entitydef_box_size("snapmaps/volume/trigger", s));
    CHECK(!sh_nav_entitydef_box_size(NULL, s));
}

/* A marked box that spells only y and z: x is the entityDef's 16, y and z are
 * the map's. Without a resolver the read is unmeasured, as in the editor. */
static void test_omitted_axis_inherits_during_conversion(void)
{
    static const char json[] =
        "{\"entities\":[{\"displayName\":\"\",\"entityDef\":{\"className\":\"idVolume_Blocking\","
        "\"inherit\":\"snapmaps/volume/blocking\",\"name\":\"\",\"state\":{\"edit\":{"
        "\"flags\":{\"noFlood\":true},\"blockDemons\":true,"
        "\"clipModelInfo\":{\"size\":{\"y\":512,\"z\":256}},"
        "\"spawnPosition\":{\"x\":0,\"y\":0,\"z\":32}}},"
        "\"targetType\":\"idDeclEntityDef\",\"~type\":\"idDeclEntityDef\"},"
        "\"layerMask\":1,\"pinned\":true,\"uniqueId\":7,\"~type\":\"idSnapEntity\"}],"
        "\"instanceEntities\":{\"keyValues\":[0,1,1],\"values\":[7],\"~type\":\"idIndexMultimap\"},"
        "\"instances\":[{\"difficultyOffset\":0,\"environmentName\":\"snapmap/base\",\"layerMask\":1,"
        "\"moduleName\":\"maps/modules/ind_dlc/ind_totally_blank_room_4x.decl\",\"orientation\":0,"
        "\"origin\":{\"x\":0,\"y\":0,\"z\":0,\"~type\":\"idVec3\"},"
        "\"restrictionMask\":0,\"~type\":\"idSnapInstance\"}],"
        "\"variables\":{\"allocCount\":[0,0,0,0,0,0,0,0,0,16],\"string\":[],"
        "\"~type\":\"idSnapVariables\"},\"~type\":\"idSnapMap\"}";
    static sh_nav_map m;
    CHECK(sh_nav_regions_read_resolved(json, sizeof json - 1, &m, inherited_only, NULL));
    CHECK(!m.invalid_geometry && !m.unmeasured && !m.truncated);
    CHECK(m.region_count == 1);
    if (m.region_count == 1) {
        const sh_nav_region *r = &m.regions[0];
        CHECK(fabsf(extent(r, 0) - 16.0f) < 0.01f);
        CHECK(fabsf(extent(r, 1) - 512.0f) < 0.01f);
        CHECK(fabsf(r->c[0][2] - (32.0f + 256.0f)) < 0.01f);
        CHECK(r->instance == 0);
    }
    CHECK(!strcmp(m.instances[0].module, "ind_dlc/ind_totally_blank_room_4x"));

    sh_nav_map_free(&m);
    CHECK(sh_nav_regions_read_resolved(json, sizeof json - 1, &m, unresolved, NULL));
    CHECK(m.unmeasured == 1 && m.region_count == 0);
    sh_nav_map_free(&m);
}

/* The converted copy carries world positions (live: the platform corner at
 * module-local (-1024, 256, 512) read back as world (128, -2688, 512) in a
 * room at (1152, -2944, 0)). Taking a converted snapshot returns its floors
 * and walls to module-local space with the room's origin and orientation; the
 * editor's own map is already local and is left alone. */
static void to_world(const sh_nav_instance *in, const float p[3], float out[3], int vector)
{
    static const int xy[8][4] = {{1,0,0,1},{0,-1,1,0},{-1,0,0,-1},{0,1,-1,0},
                                 {-1,0,0,1},{0,1,1,0},{1,0,0,-1},{0,-1,-1,0}};
    int o = in->orientation;
    out[0] = (vector ? 0.0f : in->origin[0]) + xy[o][0] * p[0] + xy[o][1] * p[1];
    out[1] = (vector ? 0.0f : in->origin[1]) + xy[o][2] * p[0] + xy[o][3] * p[1];
    out[2] = (vector ? 0.0f : in->origin[2]) + p[2];
}

static int near3(const float a[3], const float b[3])
{
    return fabsf(a[0] - b[0]) < 1e-3f && fabsf(a[1] - b[1]) < 1e-3f && fabsf(a[2] - b[2]) < 1e-3f;
}

static void test_converted_floors_return_to_module_space(void)
{
    static const float local[4][3] = {{-1024, 256, 512}, {-1024, 2304, 512},
                                      {1024, 2304, 512}, {1024, 256, 512}};
    static const float normal[3] = {0.6f, 0.8f, 0.0f};
    int o, k;
    for (o = 0; o < 8; o++) {
        sh_nav_map m;
        sh_nav_instance in[2];
        sh_nav_region r[2], w[2];
        memset(&m, 0, sizeof m); memset(in, 0, sizeof in); memset(r, 0, sizeof r);
        in[0].origin[0] = 1152; in[0].origin[1] = -2944; in[0].origin[2] = 64;
        in[0].orientation = o;
        in[1] = in[0]; in[1].unusable = 1;
        for (k = 0; k < 4; k++) to_world(&in[0], local[k], r[0].c[k], 0);
        to_world(&in[0], normal, r[0].n, 1);
        r[0].instance = 0;
        r[1] = r[0]; r[1].instance = 1;          /* an unusable room's box is left alone */
        memcpy(w, r, sizeof r);
        m.instances = in; m.instance_count = 2;
        m.regions = &r[0]; m.region_count = 2;
        m.obstacles = NULL; m.obstacle_count = 0;
        sh_nav_snapshot_map_to_local(&m);
        for (k = 0; k < 4; k++) CHECK(near3(r[0].c[k], local[k]));
        CHECK(near3(r[0].n, normal));
        CHECK(!memcmp(&r[1], &w[1], sizeof r[1]));
    }
}

/* Through sh_nav_snapshot_take: only a converted read is moved. */
static int g_world_read_local_only;
static int world_read(const void *object, int converting, sh_nav_map *out, void *ctx)
{
    static const float corner[3] = {-1024, 256, 512};
    int id = *(const int *)object;
    (void)converting; (void)ctx;
    memset(out, 0, sizeof *out);
    out->instances = (sh_nav_instance *)calloc(1, sizeof *out->instances);
    out->regions = (sh_nav_region *)calloc(1, sizeof *out->regions);
    if (!out->instances || !out->regions) abort();
    strcpy_s(out->instances[0].module, sizeof out->instances[0].module, "ind_dlc/room_a");
    out->instances[0].origin[0] = 1152; out->instances[0].origin[1] = -2944;
    out->instance_count = out->instance_cap = 1;
    out->region_count = out->region_cap = 1;
    if (id == BUILD && !g_world_read_local_only) to_world(&out->instances[0], corner, out->regions[0].c[0], 0);
    else memcpy(out->regions[0].c[0], corner, sizeof corner);
    return 1;
}

static int world_modules(const void *object, char *names, size_t cap, int count, void *ctx)
{
    (void)object; (void)count; (void)ctx;
    strcpy_s(names, cap, "maps/modules/ind_dlc/room_a.decl");
    return 1;
}

static void test_take_moves_only_the_converted_read(void)
{
    static const float corner[3] = {-1024, 256, 512};
    sh_nav_snapshot_sources src;
    sh_nav_snapshot_result r;
    sh_nav_map m;
    memset(&src, 0, sizeof src);
    src.read = world_read; src.modules = world_modules;
    /* outside the editor: converted, world -> local */
    memset(&m, 0, sizeof m);
    src.build = &BUILD; src.edit = NULL;
    CHECK(sh_nav_snapshot_take(&src, &m, &r) && r.source == SH_NAV_SNAPSHOT_CONVERTED);
    CHECK(near3(m.regions[0].c[0], corner));
    sh_nav_map_free(&m);
    /* editor preview: the edit map is local and stays so */
    memset(&m, 0, sizeof m);
    src.build = NULL; src.edit = &EDIT;
    CHECK(sh_nav_snapshot_take(&src, &m, &r) && r.source == SH_NAV_SNAPSHOT_EDITOR);
    CHECK(near3(m.regions[0].c[0], corner));
    sh_nav_map_free(&m);
    /* the edit map being converted itself: not moved */
    memset(&m, 0, sizeof m);
    g_world_read_local_only = 1;
    src.build = &EDIT; src.edit = &EDIT;
    CHECK(sh_nav_snapshot_take(&src, &m, &r) && r.source == SH_NAV_SNAPSHOT_EDITOR_SAME);
    CHECK(near3(m.regions[0].c[0], corner));
    g_world_read_local_only = 0;
    sh_nav_map_free(&m);
}

int main(void)
{
    test_no_conversion_reads_the_editor();
    test_launch_without_editor_reads_the_conversion();
    test_stale_editor_map_is_not_read();
    test_editor_play_reads_the_conversion_once();
    test_same_object();
    test_fallback_to_matching_editor_map();
    test_no_fallback_to_a_different_map();
    test_conversion_must_match_its_records();
    test_module_key();
    test_modules_match();
    test_inherited_size();
    test_omitted_axis_inherits_during_conversion();
    test_converted_floors_return_to_module_space();
    test_take_moves_only_the_converted_read();
    if (g_failed) {
        fprintf(stderr, "%d check(s) FAILED\n", g_failed);
        return 1;
    }
    printf("nav_snapshot_test: all checks passed\n");
    return 0;
}
