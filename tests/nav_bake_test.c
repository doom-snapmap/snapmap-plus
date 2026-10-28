/* Tests marked-region bake planning, resource names and load resets using
 * synthetic maps. Resource names are independent literals because a wrong
 * cooked name can silently fall through to shipped navigation. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "../src/backend/nav_bake.h"
#include "../src/backend/nav_regions.h"
#include "../src/backend/nav_traversal.h"
#include "../src/backend/aas_augment.h"
#include "../src/backend/navmesh.h"
#include "aas_fixture.h"

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
    sh_nav_bake_set_live_editor(live_count, live_valid, live_json, NULL, NULL);
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

    sh_nav_bake_set_live_editor(NULL, NULL, NULL, NULL, NULL);
}

/* Editor-side refresh must include unsaved navigation toggles in the plan. */
static void test_refresh_live_is_callable_from_the_editor(void)
{
    const int owner[1] = { 0 };
    char *json;

    printf("the editor-side refresh reads the live editor\n");
    sh_nav_bake_test_reset();
    sh_nav_bake_set_live_editor(live_count, live_valid, live_json, NULL, NULL);
    live_reset();

    json = make_map("ind_dlc/room", 1, 1, 0x1u, 0x1u, owner);
    sh_nav_bake_set_map(json, strlen(json));

    sh_nav_bake_refresh_live();
    CHECK_MSG(g_live_count_calls > 0,
              "the editor-side refresh is what reads the live surface now");

    sh_nav_bake_set_live_editor(NULL, NULL, NULL, NULL, NULL);
}

/* Without a map there is nothing to attribute a live mark to, and the refresh
 * must be a no-op rather than a scan -- it is called from the editor tick. */
static void test_refresh_live_without_a_map_is_a_no_op(void)
{
    printf("the editor-side refresh does nothing without a map\n");
    sh_nav_bake_test_reset();
    sh_nav_bake_set_live_editor(live_count, live_valid, live_json, NULL, NULL);
    live_reset();
    sh_nav_bake_refresh_live();
    CHECK_MSG(g_live_count_calls == 0, "no map means nothing to refresh");
    sh_nav_bake_set_live_editor(NULL, NULL, NULL, NULL, NULL);
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

static void report_line(const char *fmt, ...);

/* The snapshot's size reader: the second box cannot be measured yet. */
static int second_unmeasured(unsigned entity, float size[3], void *ctx)
{
    (void)ctx;
    if (entity == 1) return 0;
    size[0] = size[1] = 1024.0f; size[2] = 128.0f;
    return 1;
}
static int snapshot_partly_measured(sh_nav_map *out, void *ctx)
{
    (void)ctx;
    if (!g_snapshot_json) return 0;
    return sh_nav_regions_read_resolved(g_snapshot_json, strlen(g_snapshot_json), out,
                                        second_unmeasured, NULL);
}

/* A box the editor cannot measure keeps an ordinary read waiting for the
 * next one, but an explicit build (or Play) bakes every box it could measure
 * and names the one it left out. */
static void test_unmeasured_box_does_not_block_a_build(void)
{
    const int owner[2] = {0, 0};
    sh_nav_bake_summary s;
    printf("a box the editor cannot measure does not block an explicit build\n");
    sh_nav_bake_test_reset();
    sh_nav_bake_set_snapshot(snapshot_partly_measured, NULL);
    g_snapshot_json = make_map("ind_dlc/room", 1, 2, 3, 3, owner);
    sh_nav_bake_refresh_live();
    CHECK(!sh_nav_bake_has_map());
    sh_nav_bake_summary_get(&s);
    CHECK(strstr(s.read_reason, "measured") != NULL);
    sh_nav_bake_refresh_live_partial();
    CHECK(sh_nav_bake_has_map());
    sh_nav_bake_summary_get(&s);
    CHECK(s.rooms == 1 && s.boxes_left_out == 1);
    g_log_count = 0;
    sh_nav_bake_report(report_line);
    CHECK(log_contains("could not measure it yet"));
    sh_nav_bake_set_snapshot(NULL, NULL);
    sh_nav_bake_test_reset();
}

static void test_refused_read_holds_no_map(void)
{
    const int owner[2] = {0, 0};
    int uids[4], floors = -1, walls = -1;
    printf("a refused read after a good one reports no map until the next good read\n");
    sh_nav_bake_test_reset();
    sh_nav_bake_set_snapshot(snapshot_read, NULL);
    g_snapshot_json = make_map("ind_dlc/room", 1, 2, 3, 3, owner);
    sh_nav_bake_refresh_live();
    CHECK(sh_nav_bake_has_map());
    CHECK(sh_nav_bake_volume_uids(uids, 4) > 0);
    g_snapshot_json = NULL;
    sh_nav_bake_refresh_live();
    CHECK_MSG(!sh_nav_bake_has_map(), "the poll must not fingerprint a refused read");
    CHECK(sh_nav_bake_volume_uids(uids, 4) == 0);
    sh_nav_bake_volume_counts(&floors, &walls);
    CHECK(floors == 0 && walls == 0);
    CHECK(sh_nav_bake_instance_count() == 0);
    g_snapshot_json = make_map("ind_dlc/room", 1, 2, 3, 3, owner);
    sh_nav_bake_refresh_live();
    CHECK(sh_nav_bake_has_map());
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
    sh_nav_bake_set_live_editor(live_count, box_valid, box_json, NULL, NULL);
    g_snapshot_json = make_map("ind_dlc/room", 1, 2, 1, 3, owner);
    sh_nav_bake_refresh_live();
    sh_nav_bake_test_copy_map(&before);
    revision = sh_nav_bake_geometry_revision();
    CHECK(sh_nav_bake_refresh_volumes(&read) == 1 && read == 2);
    sh_nav_bake_test_copy_map(&after);
    CHECK(sh_nav_map_equal(&before, &after));
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
    /* A deleted box is dropped where it stands rather than costing a complete
     * snapshot. One that carried nothing leaves the map as it was. */
    g_box_missing = 101;
    CHECK(sh_nav_bake_refresh_volumes(&read) == 1);
    sh_nav_bake_test_copy_map(&after);
    CHECK(sh_nav_map_equal(&before, &after));
    /* Deleting the one that carried the region takes the region with it. */
    g_box_missing = 100;
    CHECK(sh_nav_bake_refresh_volumes(&read) == 1);
    sh_nav_bake_test_copy_map(&after);
    CHECK(after.region_count == 0 && after.obstacle_count == 0);
    g_box_missing = 0; g_box_x = 0; g_box_blocks = 3; g_box_marked = 1;
    sh_nav_bake_set_live_editor(NULL, NULL, NULL, NULL, NULL);
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
    CHECK(sh_nav_bake_prepare(no_shipped_bytes) == 1);
    CHECK(sh_nav_bake_preview(no_shipped_bytes, unexpected_line, NULL, NULL) == 1);
    sh_nav_bake_set_map(NULL, 0);
    CHECK(sh_nav_bake_prepare(no_shipped_bytes) == -1);
    CHECK(sh_nav_bake_preview(no_shipped_bytes, unexpected_line, NULL, NULL) == 1);
}

static void test_prepare_collects_worker_without_drawing(void)
{
    const int owner[] = {0};
    char *json = make_map("ind_dlc/room", 1, 1, 1, 1, owner);
    ULONGLONG deadline;
    int ready;
    sh_nav_bake_test_reset();
    sh_nav_bake_set_map(json, strlen(json));
    /* No preview callback is supplied or called. Enqueuing is not completion. */
    CHECK(sh_nav_bake_prepare(no_shipped_bytes) == 0);
    deadline = GetTickCount64() + 5000;
    do {
        Sleep(1);
        ready = sh_nav_bake_prepare(no_shipped_bytes);
    } while (!ready && GetTickCount64() < deadline);
    CHECK(ready == 1);
    CHECK(!sh_nav_bake_preview_pending());
    CHECK(sh_nav_bake_prepare(no_shipped_bytes) == 1);
    sh_nav_bake_set_map(NULL, 0);
    CHECK(sh_nav_bake_prepare(no_shipped_bytes) == -1);
    sh_nav_bake_test_reset();
}

static void test_source_invalidation_retires_cached_and_inflight_results(void)
{
    const int owner[] = {0};
    const char *name = "maps/modules/ind_dlc/room/room.aas_monster48";
    char old_name[384], new_name[384];
    static sh_nav_map before, after;
    char *json = make_map("ind_dlc/room", 1, 1, 1, 1, owner);
    unsigned long old_revision;
    unsigned char *bytes = NULL;
    size_t length = 0;
    sh_nav_bake_test_reset();
    sh_nav_bake_set_map(json, strlen(json));
    sh_nav_bake_enable_instances(1);
    old_revision = sh_nav_bake_geometry_revision();
    sh_nav_bake_test_copy_map(&before);
    CHECK(sh_nav_bake_instance_name(0, name, old_name, sizeof(old_name)));
    sh_nav_bake_test_preview_complete(old_revision, "cached source one");
    CHECK(sh_nav_bake_open(old_name, no_shipped_bytes, &bytes, &length));
    CHECK(bytes && length == 17 && !memcmp(bytes, "cached source one", 17));
    if (bytes) HeapFree(GetProcessHeap(), 0, bytes);
    sh_nav_bake_invalidate_sources();
    CHECK(sh_nav_bake_geometry_revision() != old_revision);
    sh_nav_bake_test_copy_map(&after);
    CHECK(sh_nav_map_equal(&before, &after));
    CHECK(sh_nav_bake_instance_name(0, name, new_name, sizeof(new_name)) && strcmp(new_name, old_name));
    sh_nav_bake_test_preview_complete(old_revision, "stale worker result");
    CHECK(!sh_nav_bake_open(new_name, no_shipped_bytes, &bytes, &length));
    CHECK(!bytes && !length);
    /* A source refusal can be retried after a later successful publication. */
    sh_nav_bake_invalidate_sources();
    CHECK(sh_nav_bake_instance_name(0, name, new_name, sizeof(new_name)));
    sh_nav_bake_test_preview_complete(sh_nav_bake_geometry_revision(), "cached source two");
    CHECK(sh_nav_bake_open(new_name, no_shipped_bytes, &bytes, &length));
    CHECK(bytes && length == 17 && !memcmp(bytes, "cached source two", 17));
    if (bytes) HeapFree(GetProcessHeap(), 0, bytes);
    sh_nav_bake_test_reset();
}

typedef struct guarded_read {
    HANDLE started;
    const char *name;
    unsigned char *bytes;
    size_t length;
    int result;
} guarded_read;
static DWORD WINAPI guarded_reader(LPVOID context)
{
    guarded_read *read = (guarded_read *)context;
    SetEvent(read->started);
    read->result = sh_nav_bake_open(read->name, no_shipped_bytes, &read->bytes, &read->length);
    return 0;
}
static void test_source_update_guards_navigation_reads(void)
{
    const int owner[] = {0};
    const char *raw = "maps/modules/ind_dlc/room/room.aas_monster48";
    char requested[384];
    char *json = make_map("ind_dlc/room", 1, 1, 1, 1, owner);
    unsigned long revision;
    int committed;
    sh_nav_bake_test_reset(); sh_nav_bake_set_map(json, strlen(json)); sh_nav_bake_enable_instances(1);
    revision = sh_nav_bake_geometry_revision();
    CHECK(sh_nav_bake_instance_name(0, raw, requested, sizeof(requested)));
    sh_nav_bake_test_preview_complete(revision, "held cache");
    for (committed = 0; committed < 2; committed++) {
        guarded_read read = {0};
        HANDLE thread;
        read.name = requested; read.started = CreateEventA(NULL, TRUE, FALSE, NULL);
        CHECK(read.started != NULL);
        sh_nav_bake_source_update_begin();
        thread = CreateThread(NULL, 0, guarded_reader, &read, 0, NULL); CHECK(thread != NULL);
        CHECK(WaitForSingleObject(read.started, 5000) == WAIT_OBJECT_0);
        CHECK(WaitForSingleObject(thread, 30) == WAIT_TIMEOUT);
        sh_nav_bake_source_update_end(committed);
        CHECK(WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0);
        CHECK(committed ? (!read.result && !read.bytes && !read.length) :
            (read.result && read.bytes && read.length == 10 && !memcmp(read.bytes, "held cache", 10)));
        CHECK(committed ? sh_nav_bake_geometry_revision() != revision : sh_nav_bake_geometry_revision() == revision);
        if (read.bytes) HeapFree(GetProcessHeap(), 0, read.bytes);
        CloseHandle(thread); CloseHandle(read.started);
    }
    sh_nav_bake_test_reset();
}


/* ---- degrading instead of refusing ------------------------------------ */

static int g_reads;
static unsigned char *counting_reader(const char *name, size_t *out_len)
{
    (void)name;
    g_reads++;
    if (out_len) *out_len = 0;
    return NULL;
}

static const char *ROOM48 = "maps/modules/ind_dlc/room/room.aas_monster48";
static const char *ROOM96 = "maps/modules/ind_dlc/room/room.aas_monster96";
static const char *ROOM128 = "maps/modules/ind_dlc/room/room.aas_monster128";

/* A refused monster48 bake no longer costs the copy its monster96 and
 * monster128 navigation, and a refusal already seen at this geometry is not
 * baked again: the engine gets the shipped navigation without a second try. */
static void test_classes_refuse_independently(void)
{
    const int owner[1] = { 0 };
    char *json = make_map("ind_dlc/room", 1, 1, 0x1u, 0x1u, owner);
    char name48[384], name96[384], name128[384];
    unsigned char *bytes = NULL;
    size_t len = 0;
    printf("one demon class refusing leaves the other classes of that copy alone\n");
    sh_nav_bake_test_reset();
    sh_nav_bake_set_map(json, strlen(json));
    sh_nav_bake_enable_instances(1);
    CHECK(sh_nav_bake_instance_name(0, ROOM48, name48, sizeof name48));
    /* The worker refused monster48 at this geometry. */
    sh_nav_bake_test_preview_outcome(0, SH_NAV_CLASS_REFUSED, "navigation tree deeper than 128");
    CHECK_MSG(!sh_nav_bake_instance_name(0, ROOM48, name48, sizeof name48),
              "a class known to refuse gets the shipped navigation by its own name");
    CHECK(sh_nav_bake_instance_name(0, ROOM96, name96, sizeof name96));
    CHECK(sh_nav_bake_instance_name(0, ROOM128, name128, sizeof name128));

    /* Play asking for the private monster48 name anyway: no second bake. */
    _snprintf_s(name48, sizeof name48, _TRUNCATE, "maps/smpnav/%lu/0/modules/ind_dlc/room/room.aas_monster48",
                sh_nav_bake_geometry_revision());
    g_reads = 0; g_log_count = 0;
    CHECK(!sh_nav_bake_open(name48, counting_reader, &bytes, &len));
    CHECK_MSG(g_reads == 1, "only the shipped fallback is read, the bake is not run again");
    CHECK(log_contains("refused earlier"));

    /* monster96 bakes (and here refuses for want of shipped data) on its own. */
    g_reads = 0;
    CHECK(!sh_nav_bake_open(name96, counting_reader, &bytes, &len));
    CHECK(g_reads >= 2);                 /* the bake's reads, then the fallback */
    CHECK(!sh_nav_bake_instance_name(0, ROOM96, name96, sizeof name96));
    g_reads = 0;
    _snprintf_s(name96, sizeof name96, _TRUNCATE, "maps/smpnav/%lu/0/modules/ind_dlc/room/room.aas_monster96",
                sh_nav_bake_geometry_revision());
    CHECK(!sh_nav_bake_open(name96, counting_reader, &bytes, &len));
    CHECK_MSG(g_reads == 1, "the same inputs are not baked twice");
    CHECK(sh_nav_bake_instance_name(0, ROOM128, name128, sizeof name128));

    /* A geometry change is new input: every class may try again. */
    sh_nav_bake_invalidate_sources();
    CHECK(sh_nav_bake_instance_name(0, ROOM48, name48, sizeof name48));
    CHECK(sh_nav_bake_instance_name(0, ROOM96, name96, sizeof name96));
    sh_nav_bake_test_reset();
}

static void report_line(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof line, _TRUNCATE, fmt, ap);
    va_end(ap);
    backend_log(line);
}

/* The toast names the room and the reason, and tells partial success from
 * total failure, inside the native help-row width. */
static void test_toast_names_the_room_and_reason(void)
{
    const int owner[2] = { 0, 1 };
    char *json = make_map("ind_dlc/room", 2, 2, 0x3u, 0x3u, owner);
    sh_nav_bake_summary s;
    char title[64], body[256];
    printf("the toast says how many rooms were built and why one was not\n");
    sh_nav_bake_test_reset();
    sh_nav_bake_set_map(json, strlen(json));
    sh_nav_bake_test_preview_outcome(0, SH_NAV_CLASS_BUILT, "");
    sh_nav_bake_test_preview_outcome(1, SH_NAV_CLASS_REFUSED, "its own routes already fill one area");
    sh_nav_bake_summary_get(&s);
    CHECK(s.have_map && s.rooms == 2 && s.built == 1 && s.refused == 1 && s.pending == 0);
    CHECK(strstr(s.first, "room (copy 1)") != NULL);
    sh_nav_bake_toast(&s, 1, title, sizeof title, body, sizeof body);
    CHECK(!strcmp(title, "Navmesh Partly Built"));
    CHECK(strstr(body, "1 of 2 rooms built") != NULL);
    CHECK(strstr(body, "room (copy 1): its own routes already fill one area") != NULL);
    CHECK(strlen(body) < SH_NAV_TOAST_BODY_CAP);

    /* sh_navmesh lists every class of every copy with its outcome. */
    g_log_count = 0;
    sh_nav_bake_report(report_line);
    CHECK(log_contains("monster48: keeps its shipped navigation -- its own routes"));
    CHECK(log_contains("monster96: not baked yet (baked when Play loads it)"));
    CHECK(log_contains("monster48: built"));

    sh_nav_bake_test_preview_outcome(1, SH_NAV_CLASS_PARTIAL, "12 route(s) left out at the 256-per-area limit");
    sh_nav_bake_summary_get(&s);
    sh_nav_bake_toast(&s, 1, title, sizeof title, body, sizeof body);
    CHECK(!strcmp(title, "Navmesh Partly Built"));
    CHECK(strstr(body, "2 of 2 rooms built, partly") != NULL);
    CHECK(strstr(body, "256-per-area") != NULL);

    sh_nav_bake_test_preview_outcome(1, SH_NAV_CLASS_BUILT, "");
    sh_nav_bake_summary_get(&s);
    sh_nav_bake_toast(&s, 1, title, sizeof title, body, sizeof body);
    CHECK(!strcmp(title, "Navmesh Built"));

    sh_nav_bake_test_preview_outcome(0, SH_NAV_CLASS_REFUSED, "navigation tree deeper than 128");
    sh_nav_bake_test_preview_outcome(1, SH_NAV_CLASS_REFUSED, "navigation tree deeper than 128");
    sh_nav_bake_summary_get(&s);
    sh_nav_bake_toast(&s, 1, title, sizeof title, body, sizeof body);
    CHECK(!strcmp(title, "Navmesh Build Failed"));
    CHECK(strstr(body, "No room could be built. room (copy 0)") != NULL);

    /* A very long room name and reason are shortened in the middle; the
     * pointer to sh_navmesh at the end survives. */
    {
        sh_nav_bake_summary longer = s;
        memset(longer.first, 'x', sizeof longer.first - 1);
        longer.first[sizeof longer.first - 1] = 0;
        sh_nav_bake_toast(&longer, 1, title, sizeof title, body, sizeof body);
        CHECK(strlen(body) < SH_NAV_TOAST_BODY_CAP);
        CHECK(strstr(body, "See sh_navmesh.") != NULL && strstr(body, "...") != NULL);
    }

    sh_nav_bake_toast(&s, 0, title, sizeof title, body, sizeof body);
    CHECK(!strcmp(title, "Navmesh Build Failed"));

    /* A map the read refused says why. */
    sh_nav_bake_test_reset();
    {
        static const char bad[] =
            "{\"instances\":[{\"moduleName\":\"maps/modules/ind_dlc/room.decl\"}],"
            "\"entities\":[{\"uniqueId\":1,\"entityDef\":{\"inherit\":\"snapmaps/volume/blocking\","
            "\"state\":{\"edit\":{\"blockDemons\":true,\"flags\":{\"noFlood\":true}}}}}],"
            "\"instanceEntities\":{\"keyValues\":[0],\"values\":[1]}}";
        sh_nav_bake_set_map(bad, sizeof bad - 1);
    }
    sh_nav_bake_summary_get(&s);
    CHECK(!s.have_map && strstr(s.read_reason, "room table") != NULL);
    sh_nav_bake_toast(&s, 1, title, sizeof title, body, sizeof body);
    CHECK(!strcmp(title, "Navmesh Build Failed"));
    CHECK(strstr(body, "room table") != NULL && strlen(body) < SH_NAV_TOAST_BODY_CAP);
    sh_nav_bake_test_reset();
}

/* The load and Save reads have only map text. A box the author never resized
 * carries no size there; it takes the entityDef's, so the preview survives a
 * Save. One unreadable box is left out alone and named. */
static void test_map_text_keeps_default_sized_and_drops_only_bad_boxes(void)
{
    static sh_nav_map map;
    static const char json[] =
        "{\"instances\":[{\"moduleName\":\"maps/modules/ind_dlc/room.decl\",\"orientation\":0,"
        "\"origin\":{\"x\":0,\"y\":0,\"z\":0}}],"
        "\"entities\":["
        "{\"uniqueId\":1,\"entityDef\":{\"inherit\":\"snapmaps/volume/blocking\",\"state\":{\"edit\":{"
        "\"blockDemons\":true,\"flags\":{\"noFlood\":true}}}}},"
        "{\"uniqueId\":2,\"entityDef\":{\"inherit\":\"snapmaps/volume/blocking\",\"state\":{\"edit\":{"
        "\"clipModelInfo\":{\"size\":{\"x\":0}},"
        "\"blockDemons\":true,\"flags\":{\"noFlood\":true}}}}}],"
        "\"instanceEntities\":{\"keyValues\":[0,2,2],\"values\":[1,2]}}";
    sh_nav_bake_summary s;
    float dx;
    printf("a default-sized box keeps its navigation after a Save; a bad box is left out alone\n");
    sh_nav_bake_test_reset();
    g_log_count = 0;
    sh_nav_bake_set_map(json, sizeof json - 1);
    CHECK(sh_nav_bake_has_map());
    sh_nav_bake_test_copy_map(&map);
    CHECK(map.region_count == 1 && map.dropped_total == 1);
    if (map.region_count == 1) {
        dx = map.regions[0].c[1][0] - map.regions[0].c[0][0];
        CHECK(dx == 16.0f || dx == -16.0f || map.regions[0].c[2][0] - map.regions[0].c[1][0] != 0.0f);
        CHECK(map.regions[0].c[0][2] == 64.0f);
    }
    sh_nav_map_free(&map);
    CHECK(log_contains("left out marked box 1 (id 2)"));
    sh_nav_bake_summary_get(&s);
    CHECK(s.have_map && s.rooms == 1 && s.boxes_left_out == 1);
    sh_nav_bake_test_reset();
}

/* The engine loads no tree deeper than 128. A copy that would pass it keeps
 * most of its floors and leaves the rest out, instead of losing all of them;
 * one that cannot fit even one floor is refused by name. */
static void test_tree_depth_limit_keeps_most_floors(void)
{
    sh_aug_platform p[12];
    unsigned char *module;
    size_t len = 0;
    char brief[128];
    int i, marks = 0, state;
    unsigned was;
    printf("a copy past the tree-depth limit keeps most of its floors\n");
    module = fixture_module(&len);
    CHECK(module != NULL);
    if (!module) return;
    for (i = 0; i < 12; i++) {
        float x = -1800.0f + (float)i * 300.0f;
        memset(&p[i], 0, sizeof p[i]);
        p[i].c[0][0] = x;          p[i].c[0][1] = 200;
        p[i].c[1][0] = x + 200.0f; p[i].c[1][1] = 200;
        p[i].c[2][0] = x + 200.0f; p[i].c[2][1] = -200;
        p[i].c[3][0] = x;          p[i].c[3][1] = -200;
        p[i].c[0][2] = p[i].c[1][2] = p[i].c[2][2] = p[i].c[3][2] = 16.0f;
        p[i].n[2] = 1.0f; p[i].face = 4; p[i].depth = 16.0f;
        _snprintf_s(p[i].name, sizeof p[i].name, _TRUNCATE, "volume %d", i);
    }
    /* Unlimited, the twelve fit; measure how deep they go. */
    state = sh_nav_bake_test_run("depth", p, 12, module, len, brief, sizeof brief, &marks);
    CHECK(state == SH_NAV_CLASS_BUILT);
    /* With a limit between what one floor and all twelve need, the copy sheds
     * floors rather than refusing. */
    was = sh_aug_test_set_depth_limit(0);
    for (i = 3; i < 64; i++) {
        sh_aug_test_set_depth_limit((unsigned)i);
        state = sh_nav_bake_test_run("depth", p, 12, module, len, brief, sizeof brief, &marks);
        if (state != SH_NAV_CLASS_REFUSED) break;
    }
    CHECK(state == SH_NAV_CLASS_PARTIAL);
    CHECK(strstr(brief, "left out at the 128 tree-depth limit") != NULL);
    CHECK(marks > 0);
    /* A limit nothing fits under is a named refusal. */
    sh_aug_test_set_depth_limit(1);
    state = sh_nav_bake_test_run("depth", p, 12, module, len, brief, sizeof brief, &marks);
    CHECK(state == SH_NAV_CLASS_REFUSED);
    CHECK(!strcmp(brief, "navigation tree deeper than 128"));
    sh_aug_test_set_depth_limit(was);
    free(module);
}

/* More marked boxes and rooms than the former fixed tables held. */
static void test_plans_past_the_old_tables(void)
{
    static sh_nav_map map;
    const int rooms = 300, per_room = 2;
    size_t cap = 4 * 1024 * 1024, used = 0;
    char *json = (char *)malloc(cap);
    int i, j, uid = 1;
    char name[384];
    printf("a map with 600 marked boxes in 300 rooms plans every room\n");
    if (!json) { CHECK(0); return; }
#define PUT(...) used += (size_t)_snprintf_s(json + used, cap - used, _TRUNCATE, __VA_ARGS__)
    PUT("{\"instances\":[");
    for (i = 0; i < rooms; i++)
        PUT("%s{\"moduleName\":\"maps/modules/ind_dlc/room.decl\",\"orientation\":0,"
            "\"origin\":{\"x\":%d,\"y\":0,\"z\":0}}", i ? "," : "", i * 4096);
    PUT("],\"entities\":[");
    for (i = 0; i < rooms * per_room; i++)
        PUT("%s{\"uniqueId\":%d,\"entityDef\":{\"inherit\":\"snapmaps/volume/blocking\",\"state\":{\"edit\":{"
            "\"clipModelInfo\":{\"size\":{\"x\":256,\"y\":256,\"z\":64}},"
            "\"blockDemons\":true,\"flags\":{\"noFlood\":true}}}}}", i ? "," : "", i + 1);
    PUT("],\"instanceEntities\":{\"keyValues\":[0");
    for (i = 0; i < rooms; i++) PUT(",%d", (i + 1) * per_room);
    PUT(",%d],\"values\":[", rooms * per_room);
    for (i = 0; i < rooms; i++) for (j = 0; j < per_room; j++, uid++) PUT("%s%d", uid == 1 ? "" : ",", uid);
    PUT("]}}");
#undef PUT
    sh_nav_bake_test_reset();
    sh_nav_bake_set_map(json, used);
    sh_nav_bake_enable_instances(1);
    CHECK(sh_nav_bake_has_map());
    CHECK(sh_nav_bake_instance_count() == rooms);
    sh_nav_bake_test_copy_map(&map);
    CHECK(map.region_count == rooms * per_room && map.dropped_total == 0);
    sh_nav_map_free(&map);
    CHECK(sh_nav_bake_instance_name(rooms - 1, "maps/modules/ind_dlc/room/room.aas_monster48",
                                    name, sizeof name));
    {
        sh_nav_bake_summary s;
        sh_nav_bake_summary_get(&s);
        CHECK(s.rooms == rooms);
    }
    sh_nav_bake_test_reset();
    free(json);
}

int main(void)
{
    printf("nav_bake_test\n");
    test_fast_snapshot_reads_ids_and_obstacles();
    test_empty_preview_completes();
    test_source_invalidation_retires_cached_and_inflight_results();
    test_source_update_guards_navigation_reads();
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
    test_refused_read_holds_no_map();
    test_snapshot_moves_and_changes_ownership();
    test_prepare_collects_worker_without_drawing();
    test_classes_refuse_independently();
    test_toast_names_the_room_and_reason();
    test_map_text_keeps_default_sized_and_drops_only_bad_boxes();
    test_plans_past_the_old_tables();
    test_tree_depth_limit_keeps_most_floors();
    test_unmeasured_box_does_not_block_a_build();
    sh_nav_bake_preview_stop();
    printf("%s -- %d checks, %d failed\n", g_fail ? "FAILED" : "ok", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
