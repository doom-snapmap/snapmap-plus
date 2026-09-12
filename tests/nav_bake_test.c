/* Tests marked-region bake planning, resource names and load resets using
 * synthetic maps. Resource names are independent literals because a wrong
 * cooked name can silently fall through to shipped navigation. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/backend/nav_bake.h"
#include "../src/backend/nav_regions.h"

static int g_checks = 0, g_fail = 0;

#define CHECK(cond) do {                                                      \
    g_checks++;                                                               \
    if (!(cond)) { g_fail++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define CHECK_MSG(cond, msg) do {                                             \
    g_checks++;                                                               \
    if (!(cond)) { g_fail++; printf("  FAIL %s:%d  %s -- %s\n", __FILE__, __LINE__, #cond, msg); } \
} while (0)

/* ---- what nav_bake.c links against ------------------------------------ */

static char g_log[64][512];
static int  g_log_count;

void backend_log(const char *message)
{
    if (g_log_count < (int)(sizeof g_log / sizeof g_log[0]))
        strncpy_s(g_log[g_log_count++], sizeof g_log[0], message ? message : "", _TRUNCATE);
}

static int log_contains(const char *needle)
{
    int i;
    for (i = 0; i < g_log_count; i++) if (strstr(g_log[i], needle)) return 1;
    return 0;
}

int sh_config_get_bool(const char *key, int *out_value, unsigned int *out_flags)
{
    (void)key;
    if (out_flags) *out_flags = 0;
    if (!out_value) return 0;
    *out_value = 1;
    return 1;
}

/* No engine payload is available; verify that read-back refusal is quiet
 * and leaves the map usable. */
static unsigned char *no_shipped_bytes(const char *name, size_t *out_len)
{
    (void)name;
    if (out_len) *out_len = 0;
    return NULL;
}

int sh_navmesh_validate_aas(const unsigned char *payload, size_t len,
                            char *err, size_t err_cap)
{
    (void)payload; (void)len;
    if (err && err_cap) err[0] = 0;
    return 1;
}

/* ---- synthetic maps ---------------------------------------------------- */

/* One instance of `module`, and `n` blocking volumes. `marked` and `blocks` are
 * bit masks over the volumes: bit i set means volume i carries that flag. */
static char *make_map(const char *module, int instances, int n,
                      unsigned marked, unsigned blocks, const int *owner)
{
    static char buf[65536];
    char *p = buf;
    int i;
    size_t cap = sizeof buf;

    p += _snprintf_s(p, cap - (p - buf), _TRUNCATE, "{\"instances\":[");
    for (i = 0; i < instances; i++) {
        p += _snprintf_s(p, cap - (p - buf), _TRUNCATE,
            "%s{\"moduleName\":\"maps/modules/%s.decl\",\"orientation\":0,"
            "\"origin\":{\"x\":%d.0,\"y\":0.0,\"z\":0.0}}",
            i ? "," : "", module, i * 5120);
    }
    p += _snprintf_s(p, cap - (p - buf), _TRUNCATE, "],\"entities\":[");
    for (i = 0; i < n; i++) {
        p += _snprintf_s(p, cap - (p - buf), _TRUNCATE,
            "%s{\"uniqueId\":%d,\"entityDef\":{\"inherit\":\"snapmaps/volume/blocking\","
            "\"state\":{\"edit\":{"
            "\"clipModelInfo\":{\"size\":{\"x\":1024.0,\"y\":1024.0,\"z\":128.0}},"
            "\"spawnPosition\":{\"x\":0.0,\"y\":0.0,\"z\":0.0},"
            "\"blockDemons\":%s,\"flags\":{\"noFlood\":%s}}}}}",
            i ? "," : "", 100 + i,
            (blocks & (1u << i)) ? "true" : "false",
            (marked & (1u << i)) ? "true" : "false");
    }
    p += _snprintf_s(p, cap - (p - buf), _TRUNCATE, "],\"instanceEntities\":{\"keyValues\":[");
    {
        /* CSR: one bucket per instance, then the orphan bucket. Every volume is
         * owned, so the orphan bucket is empty. */
        int off = 0, k;
        for (k = 0; k < instances; k++) {
            int c = 0, j;
            p += _snprintf_s(p, cap - (p - buf), _TRUNCATE, "%s%d", k ? "," : "", off);
            for (j = 0; j < n; j++) if (owner[j] == k) c++;
            off += c;
        }
        p += _snprintf_s(p, cap - (p - buf), _TRUNCATE, ",%d,%d],\"values\":[", off, off);
    }
    {
        int k, j, first = 1;
        for (k = 0; k < instances; k++)
            for (j = 0; j < n; j++)
                if (owner[j] == k) {
                    p += _snprintf_s(p, cap - (p - buf), _TRUNCATE, "%s%d",
                                     first ? "" : ",", 100 + j);
                    first = 0;
                }
    }
    p += _snprintf_s(p, cap - (p - buf), _TRUNCATE, "]}}");
    return buf;
}

/* ---- the name grammar -------------------------------------------------- */

static void test_name_grammar(void)
{
    char m[SH_NAVR_MODULE_CAP], c[32];
    printf("the resource-name grammar\n");

    /* The two spellings, as literal strings. The cooked one prefixes 'b' to the
     * WHOLE extension: .baas_monster48, NOT .bmonster48. */
    CHECK(sh_nav_bake_test_parse_name(
        "maps/modules/ind_dlc/ind_totally_blank_room_4x/ind_totally_blank_room_4x.aas_monster48",
        m, sizeof m, c, sizeof c) == 1);
    CHECK_MSG(strcmp(m, "ind_dlc/ind_totally_blank_room_4x") == 0, m);
    CHECK_MSG(strcmp(c, "monster48") == 0, c);

    CHECK(sh_nav_bake_test_parse_name(
        "generated/maps/modules/ind_dlc/ind_totally_blank_room_4x/ind_totally_blank_room_4x.baas_monster128",
        m, sizeof m, c, sizeof c) == 1);
    CHECK_MSG(strcmp(m, "ind_dlc/ind_totally_blank_room_4x") == 0, m);
    CHECK_MSG(strcmp(c, "monster128") == 0, c);

    /* The bug that shipped once: 'b' on the class instead of the extension. */
    CHECK_MSG(sh_nav_bake_test_parse_name(
        "generated/maps/modules/ind_dlc/room/room.bmonster48",
        m, sizeof m, c, sizeof c) == 0,
        ".bmonster48 is not a spelling the engine ever asks for");

    /* Not ours. */
    CHECK(sh_nav_bake_test_parse_name("maps/modules/a/b/c.aas_monster48",
                                      m, sizeof m, c, sizeof c) == 0);
    CHECK(sh_nav_bake_test_parse_name("generated/decls/entitydef/x.decl",
                                      m, sizeof m, c, sizeof c) == 0);
    CHECK(sh_nav_bake_test_parse_name("", m, sizeof m, c, sizeof c) == 0);
    CHECK(sh_nav_bake_test_parse_name(NULL, m, sizeof m, c, sizeof c) == 0);
}

/* ---- planning ---------------------------------------------------------- */

static void test_marked_volume_is_planned(void)
{
    const int owner[2] = { 0, 0 };
    char *json;
    printf("a marked volume plans its module\n");
    sh_nav_bake_test_reset();
    g_log_count = 0;
    json = make_map("ind_dlc/room", 1, 2, 0x1u, 0x3u, owner);
    sh_nav_bake_set_map(json, strlen(json));
    CHECK_MSG(log_contains("marks 1 volume"), "the log must say what was marked");
}

static void test_unmarked_map_is_silent(void)
{
    const int owner[2] = { 0, 0 };
    char *json;
    printf("a map with nothing marked says nothing\n");
    sh_nav_bake_test_reset();
    g_log_count = 0;
    json = make_map("ind_dlc/room", 1, 2, 0u, 0x3u, owner);
    sh_nav_bake_set_map(json, strlen(json));
    CHECK_MSG(!log_contains("marks"), "the common map must not be chatty");
}

static void test_regions_do_not_survive_the_next_map(void)
{
    const int owner[2] = { 0, 0 };
    char *json;
    unsigned char *bytes = NULL;
    size_t len = 0;
    printf("regions are cleared on every load\n");
    sh_nav_bake_test_reset();

    json = make_map("ind_dlc/room", 1, 1, 0x1u, 0x1u, owner);
    sh_nav_bake_set_map(json, strlen(json));

    /* An unmarked map must clear every region retained from the previous load. */
    g_log_count = 0;
    json = make_map("ind_dlc/room", 1, 1, 0u, 0x1u, owner);
    sh_nav_bake_set_map(json, strlen(json));
    CHECK(sh_nav_bake_open(
        "maps/modules/ind_dlc/room/room.aas_monster48", no_shipped_bytes, &bytes, &len) == 0);
    CHECK(bytes == NULL);
}

static void test_two_marked_copies_have_distinct_resources(void)
{
    const int owner[2] = { 0, 1 };
    char *json;char first[384],second[384],stale[384];
    unsigned char *bytes=NULL;size_t length=0;
    printf("identical module names and local coordinates retain exact instance ownership\n");
    sh_nav_bake_test_reset();
    g_log_count = 0;
    json = make_map("ind_dlc/room", 2, 2, 0x3u, 0x3u, owner);
    sh_nav_bake_set_map(json, strlen(json));
    sh_nav_bake_enable_instances(1);
    CHECK(sh_nav_bake_instance_name(0,"maps/modules/ind_dlc/room/room.aas_monster48",first,sizeof first));
    CHECK(sh_nav_bake_instance_name(1,"maps/modules/ind_dlc/room/room.aas_monster48",second,sizeof second));
    CHECK(strcmp(first,second)!=0);
    CHECK(!sh_nav_bake_instance_name(2,"maps/modules/ind_dlc/room/room.aas_monster48",stale,sizeof stale));
    CHECK(!sh_nav_bake_instance_name(0,"maps/modules/other/room/room.aas_monster48",stale,sizeof stale));
    CHECK(!sh_nav_bake_open("maps/modules/ind_dlc/room/room.aas_monster48",no_shipped_bytes,&bytes,&length));
    strcpy_s(stale,sizeof stale,first);
    sh_nav_bake_set_map(json,strlen(json));
    CHECK(sh_nav_bake_instance_name(0,"maps/modules/ind_dlc/room/room.aas_monster48",first,sizeof first));
    CHECK(strcmp(first,stale)!=0);
}

static void test_one_marked_copy_of_a_repeated_module_is_fine(void)
{
    const int owner[2] = { 0, 1 };
    char *json;
    printf("a repeated module with only ONE marked copy is allowed\n");
    sh_nav_bake_test_reset();
    g_log_count = 0;
    /* Twelve grid rooms, marks in one of them, is the way custom maps are built. */
    json = make_map("ind_dlc/room", 2, 2, 0x1u, 0x3u, owner);
    sh_nav_bake_set_map(json, strlen(json));
    CHECK_MSG(!log_contains("refused"), "this is the common authoring pattern");
}

static void test_unblocking_volume_is_not_a_floor(void)
{
    const int owner[1] = { 0 };
    char *json;
    unsigned char *bytes = NULL;
    size_t len = 0;
    printf("a marked volume demons fall through is not a floor\n");
    sh_nav_bake_test_reset();
    /* Marked, but blockDemons is false: navigating onto it would be a lie. */
    json = make_map("ind_dlc/room", 1, 1, 0x1u, 0u, owner);
    sh_nav_bake_set_map(json, strlen(json));
    CHECK(sh_nav_bake_open(
        "maps/modules/ind_dlc/room/room.aas_monster48", no_shipped_bytes, &bytes, &len) == 0);
}

static void test_open_is_total(void)
{
    unsigned char *bytes = (unsigned char *)1;
    size_t len = 99;
    printf("open refuses cleanly on anything it does not own\n");
    sh_nav_bake_test_reset();
    CHECK(sh_nav_bake_open(NULL, no_shipped_bytes, &bytes, &len) == 0);
    CHECK(sh_nav_bake_open("whatever", no_shipped_bytes, NULL, &len) == 0);
    CHECK(sh_nav_bake_open("whatever", no_shipped_bytes, &bytes, NULL) == 0);
    bytes = NULL; len = 0;
    CHECK(sh_nav_bake_open("maps/modules/a/b/b.aas_notaclass", no_shipped_bytes, &bytes, &len) == 0);
    CHECK(bytes == NULL && len == 0);
}

static void test_malformed_map_does_not_take_the_load_down(void)
{
    printf("a malformed map is survivable\n");
    sh_nav_bake_test_reset();
    sh_nav_bake_set_map(NULL, 0);
    sh_nav_bake_set_map("", 0);
    sh_nav_bake_set_map("{\"instances\":", 13);
    sh_nav_bake_set_map("not json at all", 15);
    CHECK(1);   /* reaching here without faulting is the assertion */
}


/* Baking runs inside the engine AAS loader, where editor entities can be
 * partially constructed. Open must use the prepared plan without calling the
 * live entity serializer; editor-side refresh supplies current state before this. */
static int g_live_count_calls, g_live_valid_calls, g_live_json_calls;

static int live_count(void *ctx) { (void)ctx; g_live_count_calls++; return 64; }
static int live_valid(int id, void *ctx) { (void)id; (void)ctx; g_live_valid_calls++; return 1; }
static int live_json(int id, char *out, int cap, void *ctx)
{
    (void)id; (void)ctx;
    g_live_json_calls++;
    if (cap > 0 && out) out[0] = 0;
    return 0;
}

static void live_reset(void)
{
    g_live_count_calls = g_live_valid_calls = g_live_json_calls = 0;
}

static void test_open_never_reads_the_live_editor(void)
{
    const int owner[1] = { 0 };
    char *json;
    unsigned char *bytes = NULL;
    size_t len = 0;

    printf("a bake never reads the live editor (issues #87/#89)\n");
    sh_nav_bake_test_reset();
    sh_nav_bake_set_live_editor(live_count, live_valid, live_json, NULL);
    live_reset();

    json = make_map("ind_dlc/room", 1, 1, 0x1u, 0x1u, owner);
    sh_nav_bake_set_map(json, strlen(json));

    /* The engine asking for all three classes, the way BuildAAS does. */
    (void)sh_nav_bake_open("maps/modules/ind_dlc/room/room.aas_monster48",
                           no_shipped_bytes, &bytes, &len);
    (void)sh_nav_bake_open("maps/modules/ind_dlc/room/room.aas_monster96",
                           no_shipped_bytes, &bytes, &len);
    (void)sh_nav_bake_open("maps/modules/ind_dlc/room/room.aas_monster128",
                           no_shipped_bytes, &bytes, &len);

    CHECK_MSG(g_live_count_calls == 0,
              "an open must not ask the editor how many entities it has");
    CHECK_MSG(g_live_valid_calls == 0,
              "an open must not probe live entity ids");
    CHECK_MSG(g_live_json_calls == 0,
              "an open must not serialize a live entity -- that is the EntityClone fault");

    sh_nav_bake_set_live_editor(NULL, NULL, NULL, NULL);
}

/* Editor-side refresh must include unsaved navigation toggles in the plan. */
static void test_refresh_live_is_callable_from_the_editor(void)
{
    const int owner[1] = { 0 };
    char *json;

    printf("the editor-side refresh reads the live editor\n");
    sh_nav_bake_test_reset();
    sh_nav_bake_set_live_editor(live_count, live_valid, live_json, NULL);
    live_reset();

    json = make_map("ind_dlc/room", 1, 1, 0x1u, 0x1u, owner);
    sh_nav_bake_set_map(json, strlen(json));

    sh_nav_bake_refresh_live();
    CHECK_MSG(g_live_count_calls > 0,
              "the editor-side refresh is what reads the live surface now");

    sh_nav_bake_set_live_editor(NULL, NULL, NULL, NULL);
}

/* Without a map there is nothing to attribute a live mark to, and the refresh
 * must be a no-op rather than a scan -- it is called from the editor tick. */
static void test_refresh_live_without_a_map_is_a_no_op(void)
{
    printf("the editor-side refresh does nothing without a map\n");
    sh_nav_bake_test_reset();
    sh_nav_bake_set_live_editor(live_count, live_valid, live_json, NULL);
    live_reset();
    sh_nav_bake_refresh_live();
    CHECK_MSG(g_live_count_calls == 0, "no map means nothing to refresh");
    sh_nav_bake_set_live_editor(NULL, NULL, NULL, NULL);
}

static const char *g_snapshot_json;
static int snapshot_read(sh_nav_map *out, void *ctx)
{
    (void)ctx;
    if (!g_snapshot_json) return 0;
    return sh_nav_regions_read(g_snapshot_json, strlen(g_snapshot_json), out);
}

static void test_complete_snapshot_tracks_creation_and_deletion(void)
{
    static sh_nav_map map;
    const int owner[2] = {0, 0};
    printf("complete snapshots update ownership, creation, deletion and the build barrier\n");
    sh_nav_bake_test_reset();
    sh_nav_bake_set_map(make_map("ind_dlc/room", 1, 0, 0, 0, owner),
                        strlen(make_map("ind_dlc/room", 1, 0, 0, 0, owner)));
    sh_nav_bake_set_snapshot(snapshot_read, NULL);
    g_snapshot_json = make_map("ind_dlc/room", 1, 2, 3, 3, owner);
    sh_nav_bake_refresh_live();
    sh_nav_bake_test_copy_map(&map); CHECK(map.region_count == 2);
    CHECK(map.instances[0].region_count == 2);
    sh_nav_bake_build_begin();
    g_snapshot_json = make_map("ind_dlc/room", 1, 0, 0, 0, owner);
    sh_nav_bake_refresh_live();
    sh_nav_bake_test_copy_map(&map); CHECK(map.region_count == 2);
    sh_nav_bake_build_end();
    sh_nav_bake_refresh_live();
    sh_nav_bake_test_copy_map(&map); CHECK(map.region_count == 0);
    CHECK(map.instances[0].region_count == 0);
    sh_nav_bake_set_snapshot(NULL, NULL);
}

static void test_snapshot_moves_and_changes_ownership(void)
{
    static sh_nav_map map;
    const int owner[2]={0,1};char before[384],after[384],*json,*value;float x;
    const char *name="maps/modules/ind_dlc/room/room.aas_monster48";
    sh_nav_bake_test_reset();sh_nav_bake_enable_instances(1);
    sh_nav_bake_set_snapshot(snapshot_read,NULL);
    g_snapshot_json=make_map("ind_dlc/room",2,2,3,3,owner);
    sh_nav_bake_refresh_live();sh_nav_bake_test_copy_map(&map);
    CHECK(map.instances[0].region_count==1&&map.instances[1].region_count==1);
    CHECK(map.regions[0].instance==0&&map.regions[1].instance==1);
    CHECK(sh_nav_bake_instance_name(0,name,before,sizeof before));x=map.regions[0].c[0][0];
    sh_nav_bake_refresh_live();
    CHECK(sh_nav_bake_instance_name(0,name,after,sizeof after));CHECK(!strcmp(before,after));
    json=(char*)g_snapshot_json;
    value=strstr(json,"\"spawnPosition\":{\"x\":0.0");CHECK(value!=NULL);
    if(value)value[strlen("\"spawnPosition\":{\"x\":")]='1';
    sh_nav_bake_refresh_live();sh_nav_bake_test_copy_map(&map);
    CHECK(map.regions[0].c[0][0]==x+1);
    CHECK(sh_nav_bake_instance_name(0,name,after,sizeof after));CHECK(strcmp(before,after)!=0);
    g_snapshot_json=NULL;sh_nav_bake_refresh_live();
    CHECK(!sh_nav_bake_instance_name(0,name,after,sizeof after));
    g_snapshot_json=make_map("ind_dlc/room",2,2,3,3,owner);sh_nav_bake_refresh_live();
    CHECK(sh_nav_bake_instance_name(1,name,after,sizeof after));
    {
        const int swapped[3]={1,0,1};
        g_snapshot_json=make_map("ind_dlc/room",2,2,3,3,swapped);
        sh_nav_bake_refresh_live();sh_nav_bake_test_copy_map(&map);
        CHECK(map.regions[0].instance==1&&map.regions[1].instance==0);
        CHECK(sh_nav_bake_instance_name(1,name,before,sizeof before));CHECK(strcmp(before,after)!=0);
        /* New copied IDs join the owner's next snapshot; no load-time ID table. */
        g_snapshot_json=make_map("ind_dlc/room",2,3,7,7,swapped);
        sh_nav_bake_refresh_live();sh_nav_bake_test_copy_map(&map);
        CHECK(map.region_count==3&&map.instances[1].region_count==2);
        CHECK(map.regions[2].instance==1);
        /* Removing a marker and changing collision each invalidate navigation. */
        g_snapshot_json=make_map("ind_dlc/room",2,3,3,7,swapped);
        sh_nav_bake_refresh_live();sh_nav_bake_test_copy_map(&map);
        CHECK(map.region_count==2&&map.obstacle_count==1);
        CHECK(map.obstacles[0].instance==1);
        g_snapshot_json=make_map("ind_dlc/room",2,3,3,3,swapped);
        sh_nav_bake_refresh_live();sh_nav_bake_test_copy_map(&map);
        CHECK(map.region_count==2&&map.obstacle_count==0);
    }
    sh_nav_bake_set_snapshot(NULL,NULL);
}

static unsigned g_box_marked = 1, g_box_blocks = 3;
static float g_box_x;
static int g_box_missing;
static int box_valid(int id, void *ctx)
{
    (void)ctx;
    return id >= 100 && id <= 101 && id != g_box_missing;
}
static int box_json(int id, char *out, int cap, void *ctx)
{
    unsigned bit = 1u << (id - 100);
    (void)ctx;
    return _snprintf_s(out, cap, _TRUNCATE,
        "{\"entityDef\":{\"inherit\":\"snapmaps/volume/blocking\",\"state\":{\"edit\":{"
        "\"clipModelInfo\":{\"size\":{\"x\":1024.0,\"y\":1024.0,\"z\":128.0}},"
        "\"spawnPosition\":{\"x\":%.1f,\"y\":0.0,\"z\":0.0},"
        "\"blockDemons\":%s,\"flags\":{\"noFlood\":%s}}}}}",
        id == 101 ? g_box_x : 0.0f,
        g_box_blocks & bit ? "true" : "false", g_box_marked & bit ? "true" : "false");
}
static void test_fast_snapshot_reads_ids_and_obstacles(void)
{
    const int owner[2] = {0, 0};
    static sh_nav_map before, after;
    unsigned long revision;
    int read = 0;
    sh_nav_bake_test_reset();
    sh_nav_bake_set_snapshot(snapshot_read, NULL);
    sh_nav_bake_set_live_editor(live_count, box_valid, box_json, NULL);
    g_snapshot_json = make_map("ind_dlc/room", 1, 2, 1, 3, owner);
    sh_nav_bake_refresh_live();
    sh_nav_bake_test_copy_map(&before);
    revision = sh_nav_bake_geometry_revision();
    CHECK(sh_nav_bake_refresh_volumes(&read) == 1 && read == 2);
    sh_nav_bake_test_copy_map(&after);
    CHECK(!memcmp(&before, &after, sizeof before));
    CHECK(sh_nav_bake_geometry_revision() == revision);
    /* A live entity reports the spawnPosition it was created with, so a moved
     * box still answers from where it started. Shape stays as the last complete
     * snapshot recorded it; only that snapshot knows where a box now stands. */
    g_box_x = 200;
    CHECK(sh_nav_bake_refresh_volumes(&read) == 1);
    sh_nav_bake_test_copy_map(&after);
    CHECK(after.obstacles[0].c[0][0] == before.obstacles[0].c[0][0]);
    CHECK(after.obstacles[0].entity == 1);
    CHECK(sh_nav_bake_geometry_revision() == revision);
    g_box_marked = 3;
    CHECK(sh_nav_bake_refresh_volumes(&read) == 1);
    sh_nav_bake_test_copy_map(&after);
    CHECK(after.region_count == 2 && after.obstacle_count == 0);
    CHECK(after.regions[1].entity == 1 && after.regions[1].marked);
    g_box_blocks = 1; g_box_marked = 1;
    CHECK(sh_nav_bake_refresh_volumes(&read) == 1);
    sh_nav_bake_test_copy_map(&before);
    CHECK(before.region_count == 1 && before.obstacle_count == 0);
    g_box_missing = 101;
    CHECK(sh_nav_bake_refresh_volumes(&read) == 0);
    sh_nav_bake_test_copy_map(&after);
    CHECK(!memcmp(&before, &after, sizeof before));
    g_box_missing = 0; g_box_x = 0; g_box_blocks = 3; g_box_marked = 1;
    sh_nav_bake_set_live_editor(NULL, NULL, NULL, NULL);
    sh_nav_bake_set_snapshot(NULL, NULL);
}

static void unexpected_line(const float a[3], const float b[3], void *ctx)
{
    (void)a; (void)b; (void)ctx; CHECK(0);
}
static void test_empty_preview_completes(void)
{
    const int owner[1] = {0};
    char *json;
    sh_nav_bake_test_reset();
    json = make_map("ind_dlc/room", 1, 0, 0, 0, owner);
    sh_nav_bake_set_map(json, strlen(json));
    CHECK(sh_nav_bake_preview(no_shipped_bytes, unexpected_line, NULL, NULL) == 1);
    sh_nav_bake_set_map(NULL, 0);
    CHECK(sh_nav_bake_preview(no_shipped_bytes, unexpected_line, NULL, NULL) == 1);
}

int main(void)
{
    printf("nav_bake_test\n");
    test_fast_snapshot_reads_ids_and_obstacles();
    test_empty_preview_completes();
    test_name_grammar();
    test_marked_volume_is_planned();
    test_unmarked_map_is_silent();
    test_regions_do_not_survive_the_next_map();
    test_two_marked_copies_have_distinct_resources();
    test_one_marked_copy_of_a_repeated_module_is_fine();
    test_unblocking_volume_is_not_a_floor();
    test_open_is_total();
    test_malformed_map_does_not_take_the_load_down();
    test_open_never_reads_the_live_editor();
    test_refresh_live_is_callable_from_the_editor();
    test_refresh_live_without_a_map_is_a_no_op();
    test_complete_snapshot_tracks_creation_and_deletion();
    test_snapshot_moves_and_changes_ownership();
    printf("%s -- %d checks, %d failed\n", g_fail ? "FAILED" : "ok", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
