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
 * log and reload outside Play; and the three example mods' scripts compile.
 *
 *   script_e2e_test.exe <tests\fixtures\scripts> */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "script/script_runtime.h"
#include "script/script_hooks.h"
#include "script/script_source.h"
#include "script/script_console.h"

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

int main(int argc, char **argv)
{
    const char *base = argc > 1 ? argv[1] : "fixtures\\scripts";
    strcpy_s(g_base, sizeof g_base, base);
    sh_script_handles_set_resolver(resolve);
    sh_script_set_map_provider(provider, NULL);
    test_play();
    test_examples_compile();
    printf(g_fail ? "script_e2e_test: FAILED\n" : "script_e2e_test: ok\n");
    return g_fail;
}
