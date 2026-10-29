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
#include "../src/backend/nav_overlay.h"
#include "../src/backend/aas_edit.h"
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
        /* Each room still holds one box at the same place in its own space, so
         * the two copies bake exactly as before: the same revision. */
        sh_nav_bake_refresh_live();sh_nav_bake_test_copy_map(&map);
        CHECK(map.instances[0].region_count==1&&map.instances[1].region_count==1);
        CHECK(map.regions[0].entity!=map.regions[1].entity);
        CHECK(sh_nav_bake_instance_name(1,name,before,sizeof before));CHECK(!strcmp(before,after));
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

/* A snapshot built directly: `count` boxes of 64 x 64 in room 0, listed in
 * order or reversed, with entity indices offset by `entity_base`. */
static struct { int count, reversed; unsigned entity_base; float jitter, moved; int which; } g_direct;
static int snapshot_direct(sh_nav_map *out, void *ctx)
{
    int i, k;
    (void)ctx;
    memset(out, 0, sizeof *out);
    out->instances = (sh_nav_instance *)calloc(1, sizeof *out->instances);
    out->regions = (sh_nav_region *)calloc((size_t)g_direct.count + 1, sizeof *out->regions);
    if (!out->instances || !out->regions) return 0;
    out->instance_count = out->instance_cap = 1;
    strcpy_s(out->instances[0].module, sizeof out->instances[0].module, "ind_dlc/room");
    out->region_cap = g_direct.count + 1;
    for (i = 0; i < g_direct.count; i++) {
        int box = g_direct.reversed ? g_direct.count - 1 - i : i;
        sh_nav_region *r = &out->regions[i];
        float x = (float)box * 300.0f + g_direct.jitter + (box == g_direct.which ? g_direct.moved : 0.0f);
        float sq[4][2] = {{0, 64}, {64, 64}, {64, 0}, {0, 0}};
        for (k = 0; k < 4; k++) {
            r->c[k][0] = x + sq[k][0]; r->c[k][1] = sq[k][1] + g_direct.jitter; r->c[k][2] = 64.0f;
        }
        r->n[2] = 1.0f; r->face = 4; r->depth = 64.0f; r->instance = 0;
        r->block_demons = 1; r->marked = 1;
        r->entity = g_direct.entity_base + (unsigned)i;
    }
    out->region_count = g_direct.count;
    out->instances[0].region_count = g_direct.count;
    return 1;
}

/* Editor Play reads the same map again through the conversion, which lists
 * the entities in another order. That must reuse the preview's bakes; a real
 * move, an added box or a removed one must not. */
static void test_reordered_read_keeps_the_bakes(void)
{
    static sh_nav_map map;
    char first[384], now[384];
    const char *name = "maps/modules/ind_dlc/room/room.aas_monster48";
    float x0;
    printf("a read listing the same boxes in another order keeps the bakes\n");
    sh_nav_bake_test_reset(); sh_nav_bake_enable_instances(1);
    memset(&g_direct, 0, sizeof g_direct);
    g_direct.count = 5; g_direct.which = -1;
    sh_nav_bake_set_snapshot(snapshot_direct, NULL);
    sh_nav_bake_refresh_live();
    CHECK(sh_nav_bake_instance_name(0, name, first, sizeof first));
    sh_nav_bake_test_copy_map(&map);
    x0 = map.regions[0].c[0][0];
    sh_nav_map_free(&map);

    /* Reversed, with other entity indices: the same geometry revision, and
     * the table keeps the first read's order with the new read's indices. */
    g_direct.reversed = 1; g_direct.entity_base = 16;
    sh_nav_bake_refresh_live();
    CHECK(sh_nav_bake_instance_name(0, name, now, sizeof now));
    CHECK_MSG(!strcmp(first, now), "reordered entities keep the revision");
    sh_nav_bake_test_copy_map(&map);
    CHECK(map.region_count == 5 && map.regions[0].c[0][0] == x0);
    CHECK(map.regions[0].entity == 16 + 4);
    sh_nav_map_free(&map);

    /* A float round trip's worth of difference is the same box. */
    g_direct.jitter = 0.004f;
    sh_nav_bake_refresh_live();
    CHECK(sh_nav_bake_instance_name(0, name, now, sizeof now));
    CHECK_MSG(!strcmp(first, now), "round-trip noise keeps the revision");

    /* A box moved two units is a change. */
    g_direct.which = 2; g_direct.moved = 2.0f;
    sh_nav_bake_refresh_live();
    CHECK(sh_nav_bake_instance_name(0, name, now, sizeof now));
    CHECK_MSG(strcmp(first, now) != 0, "a moved box takes a new revision");
    strcpy_s(first, sizeof first, now);
    /* Moving it back, in the first order again, is a change as well. */
    g_direct.moved = 0.0f; g_direct.reversed = 0;
    sh_nav_bake_refresh_live();
    CHECK(sh_nav_bake_instance_name(0, name, now, sizeof now));
    CHECK(strcmp(first, now) != 0);
    strcpy_s(first, sizeof first, now);
    /* Removing and adding a box each take a new revision. */
    g_direct.count = 4;
    sh_nav_bake_refresh_live();
    CHECK(sh_nav_bake_instance_name(0, name, now, sizeof now));
    CHECK(strcmp(first, now) != 0);
    strcpy_s(first, sizeof first, now);
    g_direct.count = 6;
    sh_nav_bake_refresh_live();
    CHECK(sh_nav_bake_instance_name(0, name, now, sizeof now));
    CHECK(strcmp(first, now) != 0);
    sh_nav_bake_set_snapshot(NULL, NULL);
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

/* sh_navmesh's per-face table: one row per volume with a floor edge, compass
 * labels in the header when every box has the same ones and in the cells
 * otherwise, all-"no floor edge" rows omitted and counted, and runs of
 * identical rows collapsed once the table would pass 40 rows. */
static void test_face_table_layout(void)
{
    static unsigned vol[60];
    static unsigned char dir[60][4], code[60][4];
    int i, longest = 0;
    printf("sh_navmesh lays the per-face report out as a table\n");
    for (i = 0; i < 3; i++) { vol[i] = 3 + i; dir[i][0] = 6; dir[i][1] = 4; dir[i][2] = 2; dir[i][3] = 0; }
    memcpy(code[0], "\x05\x05\x89\x89", 4);
    memcpy(code[1], "\x05\x01\x05\x05", 4);
    memset(code[2], 0, 4);
    g_log_count = 0;
    sh_nav_bake_test_print_faces(report_line, vol, (const unsigned char (*)[4])dir,
                                 (const unsigned char (*)[4])code, 3, 9, 2, 12);
    CHECK(log_contains("    floor climbs on 2 of 12 box face(s)"));
    CHECK(log_contains("      vol   S          W          N          E"));
    CHECK(log_contains("      3     inside     inside     9/9        9/9"));
    CHECK(log_contains("      4     inside     blocked    inside     inside"));
    CHECK(log_contains("      (1 volume(s) with no floor edge omitted)"));
    /* A rotated box carries its own labels. */
    dir[1][0] = 5; dir[1][1] = 3; dir[1][2] = 1; dir[1][3] = 7;
    g_log_count = 0;
    sh_nav_bake_test_print_faces(report_line, vol, (const unsigned char (*)[4])dir,
                                 (const unsigned char (*)[4])code, 3, 9, 2, 12);
    CHECK(log_contains("SW inside") && log_contains("NW blocked") && log_contains("S inside"));
    /* Fifty listed rows collapse runs of identical ones. */
    for (i = 0; i < 50; i++) {
        vol[i] = 10 + i; dir[i][0] = 6; dir[i][1] = 4; dir[i][2] = 2; dir[i][3] = 0;
        memset(code[i], i < 10 ? 5 : 1, 4);
        if (i == 20) memcpy(code[i], "\x89\x05\x05\x05", 4);
    }
    g_log_count = 0;
    sh_nav_bake_test_print_faces(report_line, vol, (const unsigned char (*)[4])dir,
                                 (const unsigned char (*)[4])code, 50, 9, 1, 200);
    CHECK(log_contains("      volumes 10-19 (10): all faces inside"));
    CHECK(log_contains("      volumes 20-29 (10): all faces blocked"));
    CHECK(log_contains("      30    9/9        inside     inside     inside"));
    CHECK(log_contains("      volumes 31-59 (29): all faces blocked"));
    CHECK(g_log_count < 12);
    for (i = 0; i < g_log_count; i++) if ((int)strlen(g_log[i]) > longest) longest = (int)strlen(g_log[i]);
    CHECK_MSG(longest < 110, "table lines fit the console");
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
    CHECK(log_contains("  monster48   keeps its shipped navigation"));
    CHECK(log_contains("    its own routes already fill one area"));
    CHECK(log_contains("  monster96   not baked yet (baked when Play loads it)"));
    CHECK(log_contains("  monster48   built"));
    CHECK(log_contains("  ind_dlc/room  copy 1  "));
    CHECK(log_contains("status"));
    CHECK(log_contains("  bake cache: "));
    {
        int i, longest = 0;
        for (i = 0; i < g_log_count; i++) if ((int)strlen(g_log[i]) > longest) longest = (int)strlen(g_log[i]);
        CHECK_MSG(longest < 110, "every sh_navmesh line fits the console");
    }

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
    CHECK(!strcmp(body, "The map's navmesh is ready."));
    /* Box faces no demon can climb from the floor are counted in the toast. */
    {
        sh_nav_bake_summary faced = s;
        faced.faces_climbed = 15; faced.faces_total = 24;
        sh_nav_bake_toast(&faced, 1, title, sizeof title, body, sizeof body);
        CHECK(!strcmp(title, "Navmesh Built"));
        CHECK(strstr(body, "15 of 24 box faces") != NULL);
        CHECK(strlen(body) < SH_NAV_TOAST_BODY_CAP);
    }

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

/* A copy whose file would pass the payload cap is thinned, then loses floors,
 * and is refused only when one floor alone is still over, naming the sizes. */
static void test_payload_cap_thins_instead_of_refusing(void)
{
    sh_aug_platform p[12];
    unsigned char *module;
    size_t len = 0, full;
    char brief[128];
    int i, marks = 0, state;
    printf("a copy past the payload cap is partly built, not refused\n");
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
    state = sh_nav_bake_test_run("size", p, 12, module, len, brief, sizeof brief, &marks);
    CHECK(state == SH_NAV_CLASS_BUILT);
    full = sh_nav_bake_test_last_length();
    CHECK(full > len);
    /* Just under the full size: built partly, within the cap, and the reason
     * says what the file would have been. */
    sh_aas_test_set_payload_cap(full - 64);
    state = sh_nav_bake_test_run("size", p, 12, module, len, brief, sizeof brief, &marks);
    CHECK(state == SH_NAV_CLASS_PARTIAL);
    CHECK(sh_nav_bake_test_last_length() > 0 && sh_nav_bake_test_last_length() <= full - 64);
    CHECK(strstr(brief, "file limit") != NULL);
    CHECK(strstr(sh_nav_bake_test_last_reason(), "to keep the navigation file within") != NULL);
    CHECK(strstr(sh_nav_bake_test_last_reason(), "did not produce") == NULL);
    /* Barely above the module's own file: only floors can go; at worst it is a
     * refusal that names the size and the cap, never "did not produce". */
    sh_aas_test_set_payload_cap(len + 8);
    state = sh_nav_bake_test_run("size", p, 12, module, len, brief, sizeof brief, &marks);
    CHECK(state == SH_NAV_CLASS_REFUSED);
    CHECK(strstr(brief, "navigation file over") != NULL);
    CHECK(strstr(sh_nav_bake_test_last_reason(), "MiB is served per room") != NULL);
    sh_aas_test_set_payload_cap(0);
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

/* ---- the bake cache ---------------------------------------------------- */

/* Two rooms of three 64 x 64 boxes each, built directly, with knobs for the
 * edits the cache must notice. Box `move_box` of room `move_room` is moved by
 * `move` units; room 1 sits at `room1_x` turned by `room1_turn`. */
static struct {
    int reversed, move_room, move_box, unflag_room, extra_room;
    float move, room1_x;
    int room1_turn;
} g_two;
static int snapshot_two(sh_nav_map *out, void *ctx)
{
    int room, i, k, n = 0;
    (void)ctx;
    memset(out, 0, sizeof *out);
    out->instances = (sh_nav_instance *)calloc(2, sizeof *out->instances);
    out->regions = (sh_nav_region *)calloc(8, sizeof *out->regions);
    if (!out->instances || !out->regions) return 0;
    out->instance_count = out->instance_cap = 2;
    out->region_cap = 8;
    for (room = 0; room < 2; room++) {
        strcpy_s(out->instances[room].module, sizeof out->instances[room].module, "ind_dlc/room");
        out->instances[room].origin[0] = room ? g_two.room1_x : 0.0f;
        out->instances[room].orientation = room ? g_two.room1_turn : 0;
    }
    for (i = 0; i < 8; i++) {
        int slot = g_two.reversed ? 7 - i : i;
        int r = slot / 4, box = slot % 4;
        sh_nav_region *g;
        float x, sq[4][2] = {{0, 64}, {64, 64}, {64, 0}, {0, 0}};
        if (box == 3 && g_two.extra_room != r + 1) continue;
        g = &out->regions[n];
        x = -900.0f + (float)box * 300.0f +
            (r == g_two.move_room && box == g_two.move_box ? g_two.move : 0.0f);
        for (k = 0; k < 4; k++) {
            g->c[k][0] = x + sq[k][0]; g->c[k][1] = sq[k][1]; g->c[k][2] = 64.0f;
        }
        g->n[2] = 1.0f; g->face = 4; g->depth = 64.0f; g->instance = r;
        g->block_demons = !(g_two.unflag_room == r + 1 && box == 0);
        g->marked = 1;
        g->entity = 10u + (unsigned)(g_two.reversed ? 7 - i : i);
        out->instances[r].region_count++;
        n++;
    }
    out->region_count = n;
    return 1;
}

/* The fixture module for every class, and nothing for the traversal table. */
static int g_module_reads;
static unsigned char *grid_model(size_t *out_len);
static int g_grid_models;
static unsigned char *fixture_reader(const char *name, size_t *out_len)
{
    unsigned char *module, *copy;
    size_t len = 0;
    if (out_len) *out_len = 0;
    if (strstr(name, ".bmodel")) return g_grid_models && out_len ? grid_model(out_len) : NULL;
    if (!strstr(name, "aas_monster")) return NULL;
    module = fixture_module(&len);
    if (!module) return NULL;
    /* The grid tests need a floor the boxes can cut: give the slab its corners. */
    if (g_grid_models) {
        static const float corner[4][2] = { { -2000, 2000 }, { 2000, 2000 }, { 2000, -2000 }, { -2000, -2000 } };
        char err[128];
        sh_aas *a = sh_aas_parse(module, len, err, sizeof err);
        unsigned char *written = NULL;
        size_t wlen = 0;
        int k;
        if (a) {
            for (k = 0; k < 4; k++) {
                sh_aas_put_f32(sh_aas_rec(a, SH_AAS_L_VERTICES, (unsigned)k), 0, corner[k][0]);
                sh_aas_put_f32(sh_aas_rec(a, SH_AAS_L_VERTICES, (unsigned)k), 4, corner[k][1]);
            }
            sh_aas_put_u32(sh_aas_rec(a, SH_AAS_L_AREAS, 1), 0, 8);   /* a floor area */
            written = sh_aas_write(a, &wlen);
            sh_aas_free(a);
        }
        free(module);
        if (written && out_len) *out_len = wlen;
        g_module_reads++;
        return written;
    }
    copy = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, len);
    if (copy) memcpy(copy, module, len);
    free(module);
    if (copy && out_len) *out_len = len;
    g_module_reads++;
    return copy;
}

static const char *CLASS_NAMES[3] = {
    "maps/modules/ind_dlc/room/room.aas_monster48",
    "maps/modules/ind_dlc/room/room.aas_monster96",
    "maps/modules/ind_dlc/room/room.aas_monster128",
};

/* One Play: every class of every room through the private names, as BuildAAS
 * asks. Returns how many loads ran a bake; `served` gets how many came back. */
static int play_once(int *served)
{
    int room, ci, baked = 0;
    *served = 0;
    sh_nav_bake_build_begin();
    for (room = 0; room < 2; room++)
        for (ci = 0; ci < 3; ci++) {
            char name[384];
            unsigned char *bytes = NULL;
            size_t len = 0;
            if (!sh_nav_bake_instance_name(room, CLASS_NAMES[ci], name, sizeof name)) continue;
            g_log_count = 0;
            if (sh_nav_bake_open(name, fixture_reader, &bytes, &len) && bytes && len) (*served)++;
            if (log_contains("NAV: baked '")) baked++;
            if (bytes) HeapFree(GetProcessHeap(), 0, bytes);
        }
    sh_nav_bake_build_end();
    return baked;
}

/* The editor's placement grid for the fixture room: its slab at z = 0 as two
 * triangles, wound like the stock models. Served only when asked for. */
static unsigned char *grid_model(size_t *out_len)
{
    static const float v[4][2] = { { -2000, 2000 }, { 2000, 2000 }, { 2000, -2000 }, { -2000, -2000 } };
    static const unsigned idx[6] = { 0, 1, 2, 2, 3, 0 };
    static const char mat[] = "test/aas_surface";
    size_t n = 12 + 4 + (sizeof mat - 1) + 4 + 12 + 52 + 4 * 48 + 6 * 2 + 32 + 4, at;
    unsigned char *m = (unsigned char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n);
    unsigned i;
    if (!m) return NULL;
    put32(m, 0x1b4c4d42); put32(m + 8, 1);
    at = 12;
    m[at] = (unsigned char)(sizeof mat - 1); at += 4;
    memcpy(m + at, mat, sizeof mat - 1); at += sizeof mat - 1;
    at += 4 + 12;
    put32(m + at, 4); put32(m + at + 4, 6); put32(m + at + 8, 0x1801f);
    at += 52;
    for (i = 0; i < 4; i++) { putf(m + at + i * 48, v[i][0]); putf(m + at + i * 48 + 4, v[i][1]); }
    at += 4 * 48;
    for (i = 0; i < 6; i++) put16(m + at + i * 2, idx[i]);
    at += 12;
    putf(m + at, -2000); putf(m + at + 4, -2000); putf(m + at + 12, 2000); putf(m + at + 16, 2000);
    put32(m + at + 28, 0x1b4c4d42);
    *out_len = n;
    return m;
}

static int snapshot_none(sh_nav_map *out, void *ctx)
{
    (void)ctx;
    memset(out, 0, sizeof *out);
    out->instances = (sh_nav_instance *)calloc(2, sizeof *out->instances);
    if (!out->instances) return 0;
    out->instance_count = out->instance_cap = 2;
    strcpy_s(out->instances[0].module, sizeof out->instances[0].module, "ind_dlc/room");
    strcpy_s(out->instances[1].module, sizeof out->instances[1].module, "ind_dlc/room");
    out->instances[1].origin[0] = 8192.0f;
    return 1;
}

static int wait_prepared(void)
{
    ULONGLONG deadline = GetTickCount64() + 10000;
    int ready;
    do { Sleep(1); ready = sh_nav_bake_prepare(fixture_reader); }
    while (ready == 0 && GetTickCount64() < deadline);
    return ready;
}

/* Each copy's grid model is cut where its floor was cut, for every class; the
 * preview's class bakes also serve Play; unmarking every box restores the
 * stock models. */
static void test_the_preview_cuts_the_placement_grid(void)
{
    static const char *const stock[3] = {
        "maps/modules/ind_dlc/room/room_aas_monster48.bmodel",
        "maps/modules/ind_dlc/room/room_aas_monster96.bmodel",
        "maps/modules/ind_dlc/room/room_aas_monster128.bmodel",
    };
    char name[512], cooked[600], other[512];
    unsigned char *bytes = NULL;
    size_t len = 0;
    int ci, room, named = 0, served, baked, triangles = 0;
    unsigned long long first;
    printf("the preview cuts the editor's placement grid for every class\n");
    sh_nav_bake_test_reset(); sh_nav_bake_enable_instances(1);
    memset(&g_two, 0, sizeof g_two);
    g_two.move_room = -1; g_two.room1_x = 8192.0f;
    g_grid_models = 1;
    sh_nav_bake_set_snapshot(snapshot_two, NULL);
    sh_nav_bake_refresh_live();
    CHECK(wait_prepared() == 1);
    first = sh_nav_bake_overlay_signature();
    CHECK_MSG(first != 0, "cut grid models are installed");
    for (room = 0; room < 2; room++)
        for (ci = 0; ci < 3; ci++) {
            if (!sh_nav_bake_overlay_name(room, stock[ci], name, sizeof name)) continue;
            named++;
            CHECK(!strncmp(name, "maps/smpnavgrid/", 16) && strstr(name, stock[ci]));
            CHECK(sh_nav_bake_overlay_open(name, &bytes, &len) == 1 && bytes);
            if (bytes) {
                int t = sh_nav_overlay_triangle_count(bytes, len);
                if (t > triangles) triangles = t;
                HeapFree(GetProcessHeap(), 0, bytes); bytes = NULL;
            }
        }
    CHECK_MSG(named == 6, "both copies, all three classes");
    CHECK_MSG(triangles > 2, "the slab is split around the boxes");
    CHECK(sh_nav_bake_overlay_name(0, stock[0], name, sizeof name));
    _snprintf_s(cooked, sizeof cooked, _TRUNCATE, "generated/%s", name);
    CHECK_MSG(sh_nav_bake_overlay_open(cooked, &bytes, &len) == 1, "the cooked spelling is served too");
    if (bytes) { HeapFree(GetProcessHeap(), 0, bytes); bytes = NULL; }
    strcpy_s(other, sizeof other, name);
    other[16] = other[16] == '0' ? '1' : '0';
    CHECK_MSG(sh_nav_bake_overlay_open(other, &bytes, &len) == -1 && !bytes, "an unknown model is not served");
    CHECK_MSG(sh_nav_bake_overlay_open(stock[0], &bytes, &len) == 0, "a stock name is not claimed");
    CHECK_MSG(!sh_nav_bake_overlay_name(2, stock[0], name, sizeof name), "a copy with no boxes keeps its model");
    CHECK_MSG(!sh_nav_bake_overlay_name(0, "maps/modules/ind_dlc/other/other_aas_monster48.bmodel",
                                         name, sizeof name), "another module keeps its model");
    baked = play_once(&served);
    CHECK_MSG(baked == 0 && served == 6, "Play reuses the preview's bakes of every class");

    /* Unchanged boxes keep the same models and names. */
    g_two.move_room = 1; g_two.move_box = 0; g_two.move = 64.0f;
    sh_nav_bake_refresh_live();
    CHECK(wait_prepared() == 1);
    CHECK_MSG(sh_nav_bake_overlay_signature() != first, "a moved box changes its copy's grid");
    g_two.move = 0.0f;
    sh_nav_bake_refresh_live();
    CHECK(wait_prepared() == 1);
    CHECK_MSG(sh_nav_bake_overlay_signature() == first, "and moving it back restores the same models");

    sh_nav_bake_set_snapshot(snapshot_none, NULL);
    sh_nav_bake_refresh_live();
    CHECK(wait_prepared() == 1);
    CHECK_MSG(sh_nav_bake_overlay_signature() == 0, "no marked box: no cut models");
    CHECK_MSG(!sh_nav_bake_overlay_name(0, stock[0], name, sizeof name), "the stock model is loaded again");
    g_grid_models = 0;
    sh_nav_bake_set_snapshot(NULL, NULL);
    sh_nav_bake_test_reset();
}

/* An unchanged map is served at the next Play for every class; an edit bakes
 * again exactly the room it touched. */
static void test_play_reuses_every_class_of_an_unchanged_copy(void)
{
    int served, baked, entries;
    unsigned long hits;
    printf("an unchanged copy is served from the bake cache for all three classes\n");
    sh_nav_bake_test_reset(); sh_nav_bake_enable_instances(1);
    memset(&g_two, 0, sizeof g_two);
    g_two.move_room = -1; g_two.room1_x = 8192.0f;
    sh_nav_bake_set_snapshot(snapshot_two, NULL);
    sh_nav_bake_refresh_live();

    baked = play_once(&served);
    CHECK_MSG(baked == 6 && served == 6, "the first Play bakes two rooms by three classes");
    sh_nav_bake_test_cache_stats(&entries, NULL, &hits, NULL, NULL);
    CHECK(entries == 6 && hits == 0);

    /* Play again with nothing changed, in the converted order. */
    g_two.reversed = 1;
    baked = play_once(&served);
    CHECK_MSG(baked == 0 && served == 6, "the second Play bakes nothing and serves everything");
    CHECK(log_contains("NAV: reused '") && log_contains("from an earlier bake of the same boxes"));

    /* A map reload (a launch outside the editor sets the map again) keeps it. */
    sh_nav_bake_set_snapshot(NULL, NULL);
    sh_nav_bake_set_map(NULL, 0);
    sh_nav_bake_set_snapshot(snapshot_two, NULL);
    baked = play_once(&served);
    CHECK_MSG(baked == 0 && served == 6, "a reload of the same map is served too");

    /* Moving one box of room 1 bakes room 1's three classes and serves room 0. */
    g_two.move_room = 1; g_two.move_box = 1; g_two.move = 64.0f;
    baked = play_once(&served);
    CHECK_MSG(baked == 3 && served == 6, "a moved box re-bakes its own copy only");
    baked = play_once(&served);
    CHECK_MSG(baked == 0 && served == 6, "and the Play after is served again");

    /* Moving it back is the first shape again, which is still kept. */
    g_two.move = 0.0f;
    baked = play_once(&served);
    CHECK_MSG(baked == 0, "the previous shape of a copy is still kept");

    /* An added box, a reflagged box, a moved room and a turned room each bake
     * the room they touch and nothing else. */
    g_two.extra_room = 1;
    baked = play_once(&served);
    CHECK_MSG(baked == 3, "an added box re-bakes room 0 only");
    g_two.extra_room = 0;
    CHECK(play_once(&served) == 0);   /* removed again: the kept first shape */
    g_two.unflag_room = 2;
    baked = play_once(&served);
    CHECK_MSG(baked == 3, "a box losing Block Demons re-bakes room 1 only");
    g_two.unflag_room = 0;
    g_two.room1_x = 8192.0f + 1024.0f;
    baked = play_once(&served);
    CHECK_MSG(baked == 3, "a moved room re-bakes that room only");
    g_two.room1_turn = 1;
    baked = play_once(&served);
    CHECK_MSG(baked == 3, "a turned room re-bakes that room only");
    sh_nav_bake_set_snapshot(NULL, NULL);
    sh_nav_bake_test_reset();
}

/* The preview fills the cache for monster48, and an edit to one copy rebuilds
 * only that copy's preview; Play then serves the larger classes it baked. */
static void test_preview_and_play_share_the_cache(void)
{
    int served, baked, ready;
    ULONGLONG deadline;
    printf("the preview and Play share the bake cache\n");
    sh_nav_bake_test_reset(); sh_nav_bake_enable_instances(1);
    memset(&g_two, 0, sizeof g_two);
    g_two.move_room = -1; g_two.room1_x = 8192.0f;
    sh_nav_bake_set_snapshot(snapshot_two, NULL);
    sh_nav_bake_refresh_live();
    deadline = GetTickCount64() + 10000;
    do { Sleep(1); ready = sh_nav_bake_prepare(fixture_reader); }
    while (ready == 0 && GetTickCount64() < deadline);
    CHECK(ready == 1);
    baked = play_once(&served);
    CHECK_MSG(baked == 4 && served == 6, "monster48 comes from the preview, the rest bake");
    /* Edit room 1, then Build Navmesh: the preview serves room 0 from the cache. */
    g_two.move_room = 1; g_two.move_box = 0; g_two.move = 64.0f;
    sh_nav_bake_refresh_live();
    g_log_count = 0;
    deadline = GetTickCount64() + 10000;
    do { Sleep(1); ready = sh_nav_bake_prepare(fixture_reader); }
    while (ready == 0 && GetTickCount64() < deadline);
    CHECK(ready == 1);
    CHECK(log_contains("preview reused 1 of 2"));
    baked = play_once(&served);
    CHECK_MSG(baked == 2 && served == 6, "only room 1's larger classes bake at Play");
    sh_nav_bake_set_snapshot(NULL, NULL);
    sh_nav_bake_test_reset();
}

/* A refusal is still remembered per copy and class at its revision, and it
 * leaves the cached classes of that copy alone. */
static void test_refusal_memory_survives_the_cache(void)
{
    int served, baked;
    char name[384];
    printf("a refused class is not baked again and does not disturb the cache\n");
    sh_nav_bake_test_reset(); sh_nav_bake_enable_instances(1);
    memset(&g_two, 0, sizeof g_two);
    g_two.move_room = -1; g_two.room1_x = 8192.0f;
    sh_nav_bake_set_snapshot(snapshot_two, NULL);
    sh_nav_bake_refresh_live();
    CHECK(play_once(&served) == 6);
    /* monster96 of copy 0 refuses at this geometry: no private name, no bake. */
    sh_nav_bake_test_refuse_class(1, "navigation tree deeper than 128");
    CHECK(!sh_nav_bake_instance_name(0, CLASS_NAMES[1], name, sizeof name));
    baked = play_once(&served);
    CHECK_MSG(baked == 0 && served == 5, "the refused class is skipped, the rest are served");
    /* An edit elsewhere is a new revision: the class may try again, and its
     * unchanged boxes are served from the cache rather than baked. */
    g_two.move_room = 1; g_two.move_box = 2; g_two.move = 32.0f;
    sh_nav_bake_refresh_live();
    CHECK(sh_nav_bake_instance_name(0, CLASS_NAMES[1], name, sizeof name));
    baked = play_once(&served);
    CHECK(baked == 3 && served == 6);
    sh_nav_bake_set_snapshot(NULL, NULL);
    sh_nav_bake_test_reset();
}

/* The cache keeps to its budget: older shapes go first, and a pass that needs
 * more than the budget keeps a stable part instead of evicting its own. */
static void test_cache_keeps_to_its_budget(void)
{
    int served, entries, first;
    size_t bytes, one;
    unsigned long evicted, declined;
    printf("the bake cache keeps to its memory budget\n");
    sh_nav_bake_test_reset(); sh_nav_bake_enable_instances(1);
    memset(&g_two, 0, sizeof g_two);
    g_two.move_room = -1; g_two.room1_x = 8192.0f;
    sh_nav_bake_set_snapshot(snapshot_two, NULL);
    sh_nav_bake_refresh_live();
    play_once(&served);
    sh_nav_bake_test_cache_stats(&entries, &bytes, NULL, NULL, NULL);
    CHECK(entries == 6 && bytes > 0);
    one = bytes / 6;
    /* Room for about four entries. */
    sh_nav_bake_test_reset(); sh_nav_bake_enable_instances(1);
    sh_nav_bake_set_snapshot(snapshot_two, NULL);
    sh_nav_bake_refresh_live();
    sh_nav_bake_test_set_cache_budget(one * 4 + one / 2);
    g_log_count = 0;
    first = play_once(&served);
    CHECK(first == 6 && served == 6);
    sh_nav_bake_test_cache_stats(&entries, &bytes, NULL, &evicted, &declined);
    CHECK_MSG(entries == 4 && declined == 2 && evicted == 0,
              "what does not fit is not kept; nothing this Play uses is evicted");
    CHECK(bytes <= one * 4 + one / 2);
    /* The next Play serves the same four and bakes the same two: stable. */
    CHECK(play_once(&served) == 2);
    sh_nav_bake_test_cache_stats(&entries, NULL, NULL, &evicted, NULL);
    CHECK(entries == 4 && evicted == 0);
    /* A new shape for room 1 evicts the oldest entries of the earlier Play. */
    g_two.move_room = 1; g_two.move_box = 0; g_two.move = 64.0f;
    play_once(&served);
    sh_nav_bake_test_cache_stats(&entries, &bytes, NULL, &evicted, NULL);
    CHECK(entries <= 4 && evicted > 0);
    CHECK(bytes <= one * 4 + one / 2);
    /* Source invalidation empties it. */
    sh_nav_bake_invalidate_sources();
    sh_nav_bake_test_cache_stats(&entries, &bytes, NULL, NULL, NULL);
    CHECK(entries == 0 && bytes == 0);
    sh_nav_bake_set_snapshot(NULL, NULL);
    sh_nav_bake_test_reset();
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
    test_reordered_read_keeps_the_bakes();
    test_prepare_collects_worker_without_drawing();
    test_classes_refuse_independently();
    test_toast_names_the_room_and_reason();
    test_face_table_layout();
    test_map_text_keeps_default_sized_and_drops_only_bad_boxes();
    test_plans_past_the_old_tables();
    test_tree_depth_limit_keeps_most_floors();
    test_payload_cap_thins_instead_of_refusing();
    test_unmeasured_box_does_not_block_a_build();
    test_play_reuses_every_class_of_an_unchanged_copy();
    test_preview_and_play_share_the_cache();
    test_the_preview_cuts_the_placement_grid();
    test_refusal_memory_survives_the_cache();
    test_cache_keeps_to_its_budget();
    sh_nav_bake_preview_stop();
    printf("%s -- %d checks, %d failed\n", g_fail ? "FAILED" : "ok", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
