/* script_e2e_test.c -- mod scripts end to end without the game: real mod
 * folders on disk (tests\fixtures\scripts), the folder file source, the
 * runtime, the hooks' mod table, handle table and queues, and the console.
 *
 * The engine's part is played as the hooks play it: the Play gate asks the
 * runtime to open the map (sh_script_map_open through the provider), the
 * spawn subscriber records each spawned entity with its entityDef chain
 * (sh_script_hooks_record), the frame hook calls sh_script_tick, the
 * resolver stops finding an entity when it is removed, and map end closes.
 *
 * Covers: a fixture mod whose script binds its own entityDef gets spawn and
 * think for a spawned entity, on every frame, with game time; a second
 * fixture mod whose think never returns is stopped in the frame it starts,
 * with the plain stop line naming its file and line, while the first keeps
 * running; a removed entity stops thinking; map end; the console's status,
 * log and reload outside Play; the three example mods' scripts compile; the
 * door example run through its Script nodes (the third switch fires the
 * door's node once, with the player who used it); and the spin-heal example
 * run through the entity calls against a faked engine table (it spins, heals
 * a hurt player who comes close and is gone).
 *
 *   script_e2e_test.exe <tests\fixtures\scripts> */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "script/script_runtime.h"
#include "script/script_hooks.h"
#include "script/script_source.h"
#include "script/script_console.h"
#include "script/script_node.h"
#include "script/script_api_common.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

static char g_base[MAX_PATH];

/* ------------------------------------------------------------ the engine */

static uint32_t g_dead[16];
static int g_ndead;
static int g_token;

static void *resolve(uint32_t spawn_id)
{
    int i;
    for (i = 0; i < g_ndead; i++) if (g_dead[i] == spawn_id) return NULL;
    return &g_token;
}

static void spawn(uint32_t spawn_id, const char *def, const char *inherit)
{
    const char *names[2];
    names[0] = def;
    names[1] = inherit;
    sh_script_hooks_record(spawn_id, names, inherit ? 2 : 1, NULL);
}

/* The provider the serving layer supplies: the folders set below. */
static int provider(void *context, sh_script_mod_desc *out, int cap, const sh_script_source **source, uint64_t *seed)
{
    (void)context;
    *source = sh_script_source_files();
    *seed = 42;
    return sh_script_source_map_mods(out, cap);
}

static void set_folder(const char *salt, const char *name, const char *mod_folder)
{
    char dir[MAX_PATH];
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\%s\\assets\\mods\\%s\\scripts", g_base, mod_folder, salt);
    CHECK(sh_script_source_set_folder(salt, name, dir));
}

/* ---------------------------------------------------------------- output */

static char g_out[65536];

static void cat_line(void *context, const char *line)
{
    (void)context;
    if (strlen(g_out) + strlen(line) + 2 < sizeof g_out) { strcat_s(g_out, sizeof g_out, line); strcat_s(g_out, sizeof g_out, "\n"); }
}

static const char *log_of(const char *salt)
{
    g_out[0] = 0;
    sh_script_log_read(salt, cat_line, NULL);
    return g_out;
}

static const char *console(int argc, const char *a0, const char *a1)
{
    const char *argv[2];
    argv[0] = a0;
    argv[1] = a1;
    g_out[0] = 0;
    sh_script_console(argc, argv, cat_line, NULL);
    return g_out;
}

static int count_of(const char *hay, const char *needle)
{
    int n = 0;
    const char *p = hay;
    while ((p = strstr(p, needle)) != NULL) { n++; p += strlen(needle); }
    return n;
}

/* ----------------------------------------------------------------- tests */

static void test_play(void)
{
    char stop[SH_SCRIPT_LINE_CAP];
    LARGE_INTEGER f, t0, t1;
    double ms;
    int i;

    sh_script_source_clear();
    set_folder("scprobe1", "Script Probe", "script-probe");
    set_folder("scrunaw1", "Script Runaway", "script-runaway");

    /* the Play gate: both mods are prepared, nothing runs yet */
    CHECK(sh_script_map_open() == 2);
    CHECK(sh_script_hooks_mod_count() == 2);
    CHECK(!sh_script_started());
    CHECK(strstr(console(0, NULL, NULL), "Script Probe: 1 script file ready; it runs at the first frame of Play.") != NULL);

    /* the level load spawns a marker of the probe mod and a block of the
     * runaway mod, and a stock entity nobody owns */
    spawn(101, "mods/scprobe1/probe/marker", "snapmaps/logic/relay");
    spawn(102, "mods/scrunaw1/runaway/block", "snapmaps/logic/relay");
    spawn(103, "snapmaps/logic/relay", NULL);
    CHECK(sh_script_handles_find(103, NULL) == 0);

    /* the first frame: files, map_start, the spawns, think; the runaway
     * think is stopped inside this frame */
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    sh_script_tick(0.0, 0.0);
    QueryPerformanceCounter(&t1);
    ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
    printf("  the runaway frame took %.0f ms\n", ms);
    CHECK(ms < 1000.0);
    CHECK(sh_script_started());
    CHECK(strstr(log_of("scprobe1"), "probe started\nmarker 1 spawned\n") != NULL);
    CHECK(sh_script_status("scrunaw1") == SH_SCRIPT_STOPPED);
    CHECK(sh_script_stop_line("scrunaw1", stop, sizeof stop) == 1);
    printf("  %s\n", stop);
    {
        static const char want[] = "Script stopped: Script Runaway, scripts\\runaway.lua line ";
        CHECK(strncmp(stop, want, sizeof want - 1) == 0);
    }
    CHECK(strstr(stop, "took too long") != NULL && strstr(stop, "Look for a loop that does not end.") != NULL);
    CHECK(sh_script_status("scprobe1") == SH_SCRIPT_RUNNING);

    /* the probe mod keeps running, one line a second of game time */
    for (i = 1; i <= 8; i++) sh_script_tick(i * 0.25, 0.25);
    log_of("scprobe1");
    CHECK(strstr(g_out, "marker thinking at 1.0\n") != NULL && strstr(g_out, "marker thinking at 2.0\n") != NULL);
    CHECK(count_of(g_out, "marker thinking") == 2);
    CHECK(sh_script_status("scprobe1") == SH_SCRIPT_RUNNING);
    CHECK(sh_script_status("scrunaw1") == SH_SCRIPT_STOPPED);

    /* a marker spawned during Play is driven from the next frame */
    spawn(104, "mods/scprobe1/probe/marker", NULL);
    sh_script_tick(2.25, 0.25);
    CHECK(strstr(log_of("scprobe1"), "marker 2 spawned\n") != NULL);

    /* the console while Play runs */
    console(0, NULL, NULL);
    CHECK(strstr(g_out, "Script Probe: running, 1 of 1 file loaded, 3 handlers registered, 2 entities driven.") != NULL);
    CHECK(strstr(g_out, "Script stopped: Script Runaway, scripts\\runaway.lua line ") != NULL);
    console(2, "log", "Script Probe");
    CHECK(strstr(g_out, "Script log of Script Probe:\n  probe started\n  marker 1 spawned\n") != NULL);
    CHECK(strstr(g_out, "Runaway") == NULL);
    console(2, "log", "scrunaw1");
    CHECK(strstr(g_out, "Script log of Script Runaway:\n  Script stopped: Script Runaway") != NULL);
    CHECK(strstr(console(1, "frobnicate", NULL), "sh_scripts does not know \"frobnicate\"") != NULL);

    /* both markers removed: neither thinks again, and nothing faults */
    g_dead[g_ndead++] = 101;
    g_dead[g_ndead++] = 104;
    for (i = 10; i <= 20; i++) sh_script_tick(i * 0.25, 0.25);
    CHECK(count_of(log_of("scprobe1"), "marker thinking") == 2);

    /* map end: the probe ends, the runaway stays stopped, nothing is held */
    sh_script_map_close(1);
    CHECK(sh_script_status("scprobe1") == SH_SCRIPT_ENDED);
    CHECK(sh_script_status("scrunaw1") == SH_SCRIPT_STOPPED);
    CHECK(sh_script_bytes_in_use() == 0);
    CHECK(strstr(console(0, NULL, NULL), "Script Probe: ended with the map after loading 1 of 1 file and registering 3 handlers.") != NULL);
    g_ndead = 0;
}

/* Outside Play the reload compiles every script of the open map's mods. */
static void test_examples_compile(void)
{
    sh_script_source_clear();
    set_folder("doorlogc", "Door Logic", "door-logic");
    set_folder("spinheal", "Spin Heal", "spin-heal");
    set_folder("brutemod", "Brute", "brute");
    console(1, "reload", NULL);
    printf("%s", g_out);
    CHECK(strstr(g_out, "Door Logic: 1 script file compiled without errors; it runs at the next Play.") != NULL);
    CHECK(strstr(g_out, "Spin Heal: 1 script file compiled without errors; it runs at the next Play.") != NULL);
    CHECK(strstr(g_out, "Brute: 1 script file compiled without errors; it runs at the next Play.") != NULL);

    /* the same check names a broken file and line plainly */
    sh_script_source_clear();
    set_folder("scrunaw1", "Script Runaway", "script-runaway");
    CHECK(sh_script_check_files("scrunaw1", "Script Runaway", sh_script_source_files(), cat_line, NULL) == 1);
}

/* ------------------------------------------- the door, through Script nodes */

#define NODE(uid, name) \
    "{\"displayName\":\"" name "\",\"entityDef\":{\"className\":\"idSnapMapLogic_Relay\"," \
    "\"inherit\":\"mods/doorlogc/script\",\"state\":{\"edit\":{}}},\"uniqueId\":" #uid ",\"~type\":\"idSnapEntity\"}"
static const char k_door_map[] =
    "{\"entities\":[" NODE(11, "switch_used") "," NODE(12, "open_door") "],\"~type\":\"idSnapMap\"}";

static struct { sh_script_handle h; int uid; } g_nodes[4];
static int g_nnodes;
static sh_script_handle g_fired_node, g_fired_by;
static int g_fired;

static int n_nodes(void *c, int mod, sh_script_handle *out, int cap)
{
    int i, n = 0;
    (void)c;
    for (i = 0; i < g_nnodes && n < cap; i++) if (sh_script_handle_mod(g_nodes[i].h) == mod) out[n++] = g_nodes[i].h;
    return n;
}
static int n_uid(void *c, sh_script_handle node)
{
    int i;
    (void)c;
    for (i = 0; i < g_nnodes; i++) if (g_nodes[i].h == node) return g_nodes[i].uid;
    return -1;
}
static int n_fire(void *c, sh_script_handle node, sh_script_handle by)
{
    (void)c;
    g_fired++;
    g_fired_node = node;
    g_fired_by = by;
    return 1;
}
static int n_count(void *c, int type) { (void)c; (void)type; return 0; }
static int n_get(void *c, int type, int index, sh_script_node_value *out) { (void)c; (void)type; (void)index; (void)out; return 0; }
static int n_set(void *c, int type, int index, const sh_script_node_value *in, sh_script_node_value *stored)
{
    (void)c; (void)type; (void)index; (void)in; (void)stored;
    return 0;
}
static const sh_script_node_ops k_node_ops = { NULL, n_nodes, n_uid, n_fire, n_count, n_get, n_set };

static sh_script_handle place_node(uint32_t spawn_id, int uid)
{
    const char *names[2] = { "mods/doorlogc/script", "snapmaps/logic/relay" };
    sh_script_handle h = sh_script_hooks_record(spawn_id, names, 2, NULL);
    g_nodes[g_nnodes].h = h;
    g_nodes[g_nnodes].uid = uid;
    g_nnodes++;
    return h;
}

static void test_door_nodes(void)
{
    char err[160];
    sh_script_handle sw, door, player;
    int mod, i;

    sh_script_source_clear();
    set_folder("doorlogc", "Door Logic", "door-logic");
    CHECK(sh_script_node_map_load(k_door_map, sizeof k_door_map - 1, err, sizeof err));
    CHECK(sh_script_map_open() == 1);
    mod = sh_script_hooks_mod_find("doorlogc");
    CHECK(mod >= 0);
    g_nnodes = 0;
    sw = place_node(301, 11);
    door = place_node(302, 12);
    player = sh_script_handles_ref(399);
    sh_script_tick(0.0, 0.0);
    CHECK(sh_script_status("doorlogc") == SH_SCRIPT_RUNNING);

    /* three switches wired into "switch_used": the third opens the door */
    for (i = 1; i <= 3; i++) {
        CHECK(sh_script_node_post_input(mod, sw, player, 11) == 1);
        sh_script_tick(i * 0.1, 0.1);
        CHECK(g_fired == (i == 3));
    }
    CHECK(g_fired_node == door && g_fired_by == player);
    CHECK(sh_script_status("doorlogc") == SH_SCRIPT_RUNNING);
    sh_script_map_close(1);
    sh_script_node_map_clear();
}

/* ------------------------------------ spin-heal, through the entity calls */

typedef struct went { uint32_t id; int kind, alive; float pos[3], ax[9], health, max; char def[64]; } went;
static went g_w[2];   /* 0 the pickup, 1 the player */
static int g_wn;

static went *w_find(uint32_t id)
{
    int i;
    for (i = 0; i < g_wn; i++) if (g_w[i].id == id && g_w[i].alive) return &g_w[i];
    return NULL;
}
static void *w_resolve(uint32_t id) { return w_find(id); }
static uint32_t w_spawn_id(void *e) { return e ? ((went *)e)->id : SH_SCRIPT_SPAWN_ID_NONE; }
static int w_slots(void) { return g_wn; }
static uint32_t w_slot_id(int n) { return n >= 0 && n < g_wn && g_w[n].alive ? g_w[n].id : SH_SCRIPT_SPAWN_ID_NONE; }
static int w_kind(void *e) { return e ? ((went *)e)->kind : SH_SCRIPT_KIND_GONE; }
static int w_origin(void *e, float out[3]) { memcpy(out, ((went *)e)->pos, 12); return 1; }
static int w_axis(void *e, float out[9]) { memcpy(out, ((went *)e)->ax, 36); return 1; }
static int w_place(void *e, const float *o, const float *a)
{
    if (o) memcpy(((went *)e)->pos, o, 12);
    if (a) memcpy(((went *)e)->ax, a, 36);
    return 1;
}
static int w_def(void *e, char *out, size_t cap) { strcpy_s(out, cap, ((went *)e)->def); return 1; }
static int w_event(void *ent, const char *name, const sh_script_earg *a, int n, sh_script_earg *ret, int post)
{
    went *e = (went *)ent;
    (void)n; (void)post;
    if (ret) memset(ret, 0, sizeof *ret);
    if (!strcmp(name, "getPlayerHealth") && ret) { ret->type = 'f'; ret->f = e->health; }
    else if (!strcmp(name, "getPlayerMaxHealth") && ret) { ret->type = 'f'; ret->f = e->max; }
    else if (!strcmp(name, "increaseHealth")) { e->health += a[0].f; if (e->health > e->max) e->health = e->max; }
    else if (!strcmp(name, "remove")) e->alive = 0;
    return 1;
}
static int w_no_health(void *ai, float *cur, float *max) { (void)ai; (void)cur; (void)max; return 0; }
static int w_no_heal(void *ai, float v) { (void)ai; (void)v; return 0; }
static int w_no_node(void *ai, char *out, size_t cap) { (void)ai; (void)out; (void)cap; return 0; }
static void *w_no_spawn(const char *def, const float o[3], float yaw, const char **why)
{
    (void)def; (void)o; (void)yaw;
    *why = "no spawns here";
    return NULL;
}
static int w_no_trace(const float a[3], const float b[3], uint32_t ignore, sh_script_trace *t)
{
    (void)a; (void)b; (void)ignore; (void)t;
    return 0;
}
static int g_ms;
static int w_time(int *ms) { *ms = g_ms; return 1; }
static const sh_script_engine k_world = {
    w_resolve, w_spawn_id, w_slots, w_slot_id, w_kind, w_origin, w_axis, w_place, w_def, w_event,
    w_no_health, w_no_heal, w_no_heal, w_no_node, w_no_spawn, w_no_trace, w_time
};

static double yaw_of(const went *e) { return atan2(e->ax[1], e->ax[0]) * 180.0 / 3.14159265358979323846; }

static void test_spin_heal(void)
{
    const char *names[1] = { "mods/spinheal/pickups/spin_heal" };
    went *pickup = &g_w[0], *player = &g_w[1];
    int i;

    memset(g_w, 0, sizeof g_w);
    g_wn = 2;
    pickup->id = 501; pickup->kind = SH_SCRIPT_KIND_OTHER; pickup->alive = 1;
    pickup->ax[0] = pickup->ax[4] = pickup->ax[8] = 1;
    strcpy_s(pickup->def, sizeof pickup->def, names[0]);
    player->id = 502; player->kind = SH_SCRIPT_KIND_PLAYER; player->alive = 1;
    player->ax[0] = player->ax[4] = player->ax[8] = 1;
    player->pos[0] = 200; player->health = 50; player->max = 100;
    strcpy_s(player->def, sizeof player->def, "player/sp");
    sh_script_handles_set_resolver(w_resolve);
    sh_script_api_set_engine(&k_world);

    sh_script_source_clear();
    set_folder("spinheal", "Spin Heal", "spin-heal");
    CHECK(sh_script_map_open() == 1);
    sh_script_hooks_record(pickup->id, names, 1, NULL);
    g_ms = 0;
    sh_script_tick(0.0, 0.0);
    CHECK(sh_script_status("spinheal") == SH_SCRIPT_RUNNING);

    /* half a second at a time: a quarter turn every second, the player far off */
    for (i = 1; i <= 2; i++) {
        g_ms = i * 500;
        sh_script_api_frame();
        sh_script_tick(i * 0.5, 0.5);
    }
    printf("  the pickup faces %.1f degrees\n", yaw_of(pickup));
    CHECK(fabs(yaw_of(pickup) - 90.0) < 0.5);
    CHECK(player->health == 50 && pickup->alive);

    /* the player comes within 40 units: 25 back, and the pickup is gone */
    player->pos[0] = 30;
    g_ms = 1500;
    sh_script_api_frame();
    sh_script_tick(1.5, 0.5);
    CHECK(player->health == 75 && !pickup->alive);
    g_ms = 2000;
    sh_script_api_frame();
    sh_script_tick(2.0, 0.5);
    CHECK(player->health == 75);
    CHECK(sh_script_status("spinheal") == SH_SCRIPT_RUNNING);
    if (sh_script_status("spinheal") != SH_SCRIPT_RUNNING) {
        char stop[SH_SCRIPT_LINE_CAP];
        sh_script_stop_line("spinheal", stop, sizeof stop);
        printf("  %s\n", stop);
    }
    sh_script_map_close(1);
    sh_script_api_set_engine(NULL);
    sh_script_handles_set_resolver(resolve);
}

int main(int argc, char **argv)
{
    const char *base = argc > 1 ? argv[1] : "fixtures\\scripts";
    strcpy_s(g_base, sizeof g_base, base);
    sh_script_handles_set_resolver(resolve);
    sh_script_set_map_provider(provider, NULL);
    sh_script_node_set_ops(&k_node_ops);
    CHECK(sh_script_node_api_register());
    sh_script_api_install(NULL, 0, NULL);   /* the entity, animation, world and time calls */
    test_play();
    test_examples_compile();
    test_door_nodes();
    test_spin_heal();
    printf(g_fail ? "script_e2e_test: FAILED\n" : "script_e2e_test: ok\n");
    return g_fail;
}
