/* script_api_ai_test.c -- the AI calls of mod scripts
 * (src/backend/script/script_api_ai.c), with no engine: the runtime runs
 * real scripts and a fake engine stands behind the seam, recording every
 * event the calls send.
 *
 * Covers: the event catalogue against the build 13954591 event registry
 * (tests/fixtures/scripts/event_registry_13954591.txt, given on the command
 * line): every event the calls send exists there with the argument format
 * the calls build; argument fitting; move completion from the move FSM's
 * status word; angle and ring geometry; and, through scripts, what each call
 * sends (the park, the abort flags once, walk state, the move, facing,
 * enemy, animation, weapon, laser, focus tag), what each query answers
 * (enemy, sight, awareness, path length while searching and once found,
 * find_position, trigger state), the refusals (another mod's demon, an
 * entity that is not a demon, a bad position, an unknown option) as plain
 * stop lines, and a removed demon answering nil. */
#include <windows.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "script/script_runtime.h"
#include "script/script_hooks.h"
#include "script/script_api_ai.h"
#include "../src/third_party/lua/lauxlib.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

/* ------------------------------------------------------------ the source */

typedef struct { char salt[16]; char path[64]; char *text; } tfile;
static tfile g_files[16];
static int g_nfiles;

static void files_clear(void)
{
    int i;
    for (i = 0; i < g_nfiles; i++) free(g_files[i].text);
    g_nfiles = 0;
}

static void file_add(const char *salt, const char *path, const char *text)
{
    tfile *f = &g_files[g_nfiles++];
    strcpy(f->salt, salt);
    strcpy(f->path, path);
    f->text = _strdup(text);
}

static int src_list(void *ctx, const char *salt, sh_script_file_fn fn, void *fnctx)
{
    int i, n = 0;
    (void)ctx;
    for (i = 0; i < g_nfiles; i++) if (!strcmp(g_files[i].salt, salt)) { fn(fnctx, g_files[i].path); n++; }
    return n;
}

static int src_read(void *ctx, const char *salt, const char *rel, char **text, size_t *len)
{
    int i;
    (void)ctx;
    for (i = 0; i < g_nfiles; i++)
        if (!strcmp(g_files[i].salt, salt) && !_stricmp(g_files[i].path, rel)) {
            *text = _strdup(g_files[i].text);
            *len = strlen(*text);
            return 1;
        }
    return 0;
}

static void src_release(void *ctx, char *text) { (void)ctx; free(text); }
static const sh_script_source g_src = { NULL, src_list, src_read, src_release };

/* ------------------------------------------------------- the fake engine */

typedef struct fent {
    int   live, is_ai;
    float origin[3], yaw;
    int   enemy;           /* spawn id of the enemy, 0 none */
    int   awareness;
    int   move_status;
    int   trigger;
    int   visible;         /* isEntityVisible answer */
    int   parked;
    int   path_calls;      /* path() calls so far; searching until path_ready of them */
    int   path_ready;
    int   path_none;       /* answer "no path" */
} fent;

#define NENT 16
static fent g_ent[NENT];
static uint32_t g_frame;
static char g_events[16384];

static void *fake_resolve(uint32_t id) { return id < NENT && g_ent[id].live ? &g_ent[id] : NULL; }
static int idx_of(void *e) { return (int)((fent *)e - g_ent); }

static void *f_entity(sh_script_handle h) { return sh_script_handle_entity(h); }
static sh_script_handle f_handle_of(void *e)
{
    uint32_t id = (uint32_t)idx_of(e);
    sh_script_handle h = sh_script_handles_find(id, NULL);
    return h ? h : sh_script_handles_ref(id);
}
static int f_is_ai(void *e) { return ((fent *)e)->is_ai; }
static int f_origin(void *e, float out[3]) { memcpy(out, ((fent *)e)->origin, 12); return 1; }
static int f_yaw(void *e, float *d) { *d = ((fent *)e)->yaw; return 1; }

static void ev_cat(const char *s)
{
    if (strlen(g_events) + strlen(s) + 2 < sizeof g_events) strcat(g_events, s);
}

static int f_event(void *e, int ev, const sh_ai_arg *args, int n, double *ret)
{
    char line[256];
    int i;
    CHECK(sh_ai_args_fit(SH_AI_EVENTS[ev].format, args, n));
    snprintf(line, sizeof line, "%d:%s(", idx_of(e), SH_AI_EVENTS[ev].name);
    ev_cat(line);
    for (i = 0; i < n; i++) {
        const sh_ai_arg *a = &args[i];
        switch (a->type) {
        case 'v': snprintf(line, sizeof line, "%g %g %g", a->v[0], a->v[1], a->v[2]); break;
        case 'i': case 'b': snprintf(line, sizeof line, "%d", a->i); break;
        case 'f': snprintf(line, sizeof line, "%g", a->f); break;
        case 's': case '1': snprintf(line, sizeof line, "%s", a->s); break;
        case 'e': snprintf(line, sizeof line, "e%d", idx_of(a->ent)); break;
        case '4': snprintf(line, sizeof line, "noanim"); break;
        default: snprintf(line, sizeof line, "?");
        }
        ev_cat(i ? "," : "");
        ev_cat(line);
    }
    ev_cat(")\n");
    if (ret) {
        if (ev == SH_AI_EV_IS_ENTITY_VISIBLE) *ret = ((fent *)e)->visible;
        else if (ev == SH_AI_EV_HAS_AWARENESS_OF_ENEMY) *ret = ((fent *)e)->enemy != 0;
        else *ret = 0;
    }
    return 1;
}

static int f_park(void *e) { if (!((fent *)e)->parked) ev_cat("park\n"); ((fent *)e)->parked = 1; return 1; }
static void *f_enemy(void *e) { int id = ((fent *)e)->enemy; return id ? &g_ent[id] : NULL; }
static int f_awareness(void *ai, void *t) { (void)t; return ((fent *)ai)->awareness; }
static int f_move_status(void *e) { return ((fent *)e)->move_status; }
static int f_trigger(void *e) { return ((fent *)e)->trigger; }
static int f_path(void *ai, const float goal[3], int key, float *len)
{
    fent *f = (fent *)ai;
    (void)key;
    if (f->path_none) return 2;
    if (++f->path_calls <= f->path_ready) return 1;
    *len = (float)(fabs(goal[0] - f->origin[0]) + fabs(goal[1] - f->origin[1]));
    return 0;
}
static uint32_t f_frame(void) { return g_frame; }

static const sh_script_ai_engine g_fake = {
    f_entity, f_handle_of, f_is_ai, f_origin, f_yaw, f_event, f_park, f_enemy, f_awareness,
    f_move_status, f_trigger, f_path, f_frame
};

/* ---------------------------------------------------------------- helpers */

static char g_log[32768];
static void log_cat(void *ctx, const char *line)
{
    (void)ctx;
    if (strlen(g_log) + strlen(line) + 2 < sizeof g_log) { strcat(g_log, line); strcat(g_log, "\n"); }
}
static const char *log_of(const char *salt)
{
    g_log[0] = 0;
    sh_script_log_read(salt, log_cat, NULL);
    return g_log;
}
static char g_stop[SH_SCRIPT_LINE_CAP];
static const char *stop_of(const char *salt)
{
    sh_script_stop_line(salt, g_stop, sizeof g_stop);
    return g_stop;
}

/* ent(id): a handle to any entity (as a hook hands one over) */
static int t_ent(lua_State *L)
{
    uint32_t id = (uint32_t)luaL_checkinteger(L, 1);
    sh_script_handle h = sh_script_handles_find(id, NULL);
    sh_script_push_entity(L, h ? h : sh_script_handles_ref(id));
    return 1;
}

static void spawn(uint32_t id, const char *name)
{
    const char *names[1];
    names[0] = name;
    CHECK(sh_script_hooks_record(id, names, 1, NULL) != 0);
}

/* Two mods; ids: 1 an AI of mod A, 2 the player, 3 a non-AI of mod A, 4 an
 * AI of mod B. */
static void world(void)
{
    memset(g_ent, 0, sizeof g_ent);
    g_events[0] = 0;
    g_frame = 0;
    g_ent[1].live = 1; g_ent[1].is_ai = 1;
    g_ent[2].live = 1; g_ent[2].origin[0] = 100; g_ent[2].origin[1] = 100;
    g_ent[3].live = 1;
    g_ent[4].live = 1; g_ent[4].is_ai = 1;
}

static int start(const char *a_body)
{
    sh_script_mod_desc d[2] = { { "aaaaaaaa", "Brute Mod" }, { "bbbbbbbb", "Other Mod" } };
    files_clear();
    file_add("aaaaaaaa", "main.lua", a_body);
    file_add("bbbbbbbb", "main.lua", "api = 1\n");
    world();
    sh_script_ai_reset();
    if (sh_script_map_start(d, 2, &g_src, 1, 0.0) != 2) return 0;
    spawn(1, "mods/aaaaaaaa/ai/brute");
    spawn(3, "mods/aaaaaaaa/prop/rock");
    spawn(4, "mods/bbbbbbbb/ai/imp");
    return 1;
}

static void tick(void)
{
    g_frame++;
    sh_script_tick(g_frame * 0.1, 0.1);
}

/* ------------------------------------------------------------------ tests */

static void test_catalogue(const char *registry_path)
{
    static char text[200000];
    FILE *f = fopen(registry_path, "rb");
    size_t n;
    int ev, rows = 0;
    CHECK(f != NULL);
    if (!f) return;
    n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[n] = 0;
    for (ev = 0; ev < SH_AI_EV_COUNT; ev++) {
        char *line = text, *next;
        int found = 0;
        for (; line && *line; line = next) {
            char name[96], fmt[32];
            int num;
            next = strchr(line, '\n');
            if (next) next++;
            if (*line == '#') continue;
            if (sscanf(line, "%d %95s %31s", &num, name, fmt) != 3) continue;
            if (ev == 0) rows++;
            if (strcmp(name, SH_AI_EVENTS[ev].name)) continue;
            found = 1;
            if (!strcmp(fmt, "-")) fmt[0] = 0;
            if (!sh_ai_event_format_ok(ev, fmt)) printf("format of %s is %s in the registry\n", name, fmt);
            CHECK(sh_ai_event_format_ok(ev, fmt));
        }
        if (!found) printf("%s is not in the registry\n", SH_AI_EVENTS[ev].name);
        CHECK(found);
    }
    CHECK(rows == 1611);
    CHECK(!sh_ai_event_format_ok(SH_AI_EV_MOVE_TO_POINT, "vifb"));
    CHECK(!sh_ai_event_format_ok(-1, ""));
}

static void test_args(void)
{
    sh_ai_arg a[3];
    int dummy;
    memset(a, 0, sizeof a);
    a[0].type = 'b'; a[1].type = 's'; a[1].s = "tag";
    CHECK(sh_ai_args_fit("bs", a, 2));
    CHECK(!sh_ai_args_fit("bs", a, 1));
    CHECK(!sh_ai_args_fit("is", a, 2));
    a[1].s = NULL;
    CHECK(!sh_ai_args_fit("bs", a, 2));
    a[0].type = '4'; a[0].ent = NULL; a[1].type = 's'; a[1].s = "x";
    CHECK(sh_ai_args_fit("4s", a, 2));
    a[0].ent = &dummy;
    CHECK(!sh_ai_args_fit("4s", a, 2));
    a[0].type = 'e';
    CHECK(sh_ai_args_fit("e", a, 1));
    a[0].ent = NULL;
    CHECK(!sh_ai_args_fit("e", a, 1));
    CHECK(sh_ai_args_fit("", NULL, 0));
}

static void test_move_state(void)
{
    sh_ai_move m;
    memset(&m, 0, sizeof m);
    sh_ai_move_issue(&m, 10);
    CHECK(sh_ai_move_step(&m, 0, 10) == SH_AI_MOVE_RUNNING);   /* the command takes effect next think */
    CHECK(sh_ai_move_step(&m, 1, 11) == SH_AI_MOVE_RUNNING);
    CHECK(sh_ai_move_step(&m, 3, 20) == SH_AI_MOVE_RUNNING);
    CHECK(sh_ai_move_step(&m, 0, 21) == SH_AI_MOVE_ARRIVED);
    CHECK(sh_ai_move_step(&m, 2, 22) == SH_AI_MOVE_ARRIVED);   /* stays done until the next command */
    sh_ai_move_issue(&m, 30);
    CHECK(sh_ai_move_step(&m, 1, 31) == SH_AI_MOVE_RUNNING);
    CHECK(sh_ai_move_step(&m, 6, 32) == SH_AI_MOVE_FAILED);
    sh_ai_move_issue(&m, 40);
    CHECK(sh_ai_move_step(&m, 0, 41) == SH_AI_MOVE_RUNNING);
    CHECK(sh_ai_move_step(&m, 0, 40 + SH_AI_MOVE_SETTLE) == SH_AI_MOVE_ARRIVED);   /* already there */
}

static void test_geometry(void)
{
    float from[3] = { 0, 0, 0 }, left[3] = { 0, 10, 0 }, behind[3] = { -10, 0, 0 }, ring[4][3];
    float c[3] = { 100, 0, 5 }, toward[3] = { 0, 0, 0 };
    CHECK(fabsf(sh_ai_angle_to(from, 0, left) - 90) < 1e-3f);
    CHECK(fabsf(sh_ai_angle_to(from, 90, left)) < 1e-3f);
    CHECK(fabsf(sh_ai_angle_to(from, 0, behind) - 180) < 1e-3f);
    CHECK(fabsf(sh_ai_angle_to(from, 270, left) - 180) < 1e-3f || fabsf(sh_ai_angle_to(from, 270, left) + 180) < 1e-3f);
    sh_ai_ring(c, toward, 50, 4, ring);
    CHECK(fabsf(ring[0][0] - 50) < 1e-3f && fabsf(ring[0][1]) < 1e-3f && ring[0][2] == 5);
    CHECK(fabsf(ring[2][0] - 150) < 1e-3f);
    CHECK(sh_ai_position_score(300, 400, 1000, 0.5f) == 600);
    CHECK(!strcmp(sh_ai_awareness_name(3), "confirmed") && !strcmp(sh_ai_awareness_name(0), "unaware"));
    CHECK(sh_ai_awareness_name(7) == NULL);
}

static void test_commands(void)
{
    const char *log;
    CHECK(start(
        "api = 1\n"
        "local step = 0\n"
        "on.think('ai/brute', function(self)\n"
        "  step = step + 1\n"
        "  if step == 1 then\n"
        "    assert(self:move_to({x = 10, y = 20, z = 0}) == true)\n"
        "    print('done1 ' .. tostring(self:move_done()))\n"
        "  elseif step == 2 then\n"
        "    self:move_to({30, 40, 0}, 24, { run = true })\n"
        "    self:face(ent(2))\n"
        "    self:face({x = 0, y = 50, z = 0})\n"
        "    ai_anim(self, 'melee')\n"
        "    ai_anim(self, 'attacks/throw', 'attacks/throw_into')\n"
        "    ai_anim(self, 'idle', nil, 200)\n"
        "    self:set_enemy()\n"
        "    self:set_enemy(ent(2))\n"
        "    self:aim_at(ent(2))\n"
        "    self:aim_at(nil)\n"
        "    self:trigger(true)\n"
        "    self:trigger(false)\n"
        "    self:equip(2)\n"
        "    self:laser('laser_sources', true)\n"
        "    self:laser_sweep('rain')\n"
        "    self:laser_state('main_beam', false)\n"
        "    self:focus_tag('focus_aim_idle')\n"
        "    self:focus_tag(nil)\n"
        "    self:stop()\n"
        "    print('stopped ' .. tostring(self:move_done()))\n"
        "  end\n"
        "end)\n"));
    tick();
    tick();
    log = log_of("aaaaaaaa");
    CHECK(strstr(log, "done1 false") != NULL);
    CHECK(strstr(log, "stopped true") != NULL);
    CHECK(sh_script_status("aaaaaaaa") == SH_SCRIPT_RUNNING);
    CHECK(g_ent[1].parked);
    /* the park and the abort flags come once, before the first command */
    CHECK(strstr(g_events, "park\n1:setScriptAbort(0,0)\n1:setScriptAbortFlags(0,0)\n"
                           "1:ssaction_SetWalkState(1)\n1:ssaction_MoveToPoint(10 20 0,2,0.1,0,0 0 0)\n") == g_events);
    CHECK(strstr(strstr(g_events, "MoveToPoint") + 1, "setScriptAbort") == NULL);
    CHECK(strstr(g_events, "1:ssaction_SetWalkState(2)\n1:ssaction_MoveToPoint(30 40 0,2,24,0,0 0 0)\n") != NULL);
    CHECK(strstr(g_events, "1:setFocus(e2,0,1,3)\n") != NULL);
    CHECK(strstr(g_events, "1:ssaction_MoveToPoint(0 0 0,2,0.1,1,0 1 0)\n") != NULL);
    CHECK(strstr(g_events, "1:ssaction_ChangeAnimState(0,melee,0)\n") != NULL);
    CHECK(strstr(g_events, "1:ssaction_ChangeAnimStateVia(0,attacks/throw,attacks/throw_into,0)\n") != NULL);
    CHECK(strstr(g_events, "1:ssaction_ForceAnimState(0,idle,200,0)\n") != NULL);
    CHECK(strstr(g_events, "1:setPlayerEnemy(0)\n1:setEnemy(e2,1)\n") != NULL);
    CHECK(strstr(g_events, "1:setFocus(e2,1,1,3)\n1:clearAimFocus()\n") != NULL);
    CHECK(strstr(g_events, "1:pullTriggerRight()\n1:releaseTriggerRight()\n1:equipInventoryItemByIndex(2)\n") != NULL);
    CHECK(strstr(g_events, "1:setLaserState(1,laser_sources)\n1:ae_startLaserSweep(noanim,rain)\n"
                           "1:ae_setLaserState(noanim,0,main_beam)\n") != NULL);
    CHECK(strstr(g_events, "1:ae_setOverrideFocusTagName(noanim,0,focus_aim_idle)\n1:ae_clearOverrideFocusTag(noanim,0)\n") != NULL);
    CHECK(strstr(g_events, "1:ssaction_Idle(0,1)\n") != NULL);
    sh_script_map_end();
}

static void test_move_done_and_follow(void)
{
    const char *log;
    CHECK(start(
        "api = 1\n"
        "local step = 0\n"
        "on.think('ai/brute', function(self)\n"
        "  step = step + 1\n"
        "  if step == 1 then self:move_to_entity(ent(2)) end\n"
        "  if step == 3 then self:move_to_entity(ent(2)) end\n"
        "  local d, f = self:move_done()\n"
        "  print('step ' .. step .. ' ' .. tostring(d) .. ' ' .. tostring(f))\n"
        "end)\n"));
    tick();                                   /* step 1: order, status 0 */
    g_ent[1].move_status = 2; tick();         /* step 2: under way */
    g_ent[2].origin[0] = 110; tick();         /* step 3: target moved 10: no new order */
    g_ent[2].origin[0] = 200; g_ent[1].move_status = 0; tick();   /* step 4: arrived */
    log = log_of("aaaaaaaa");
    CHECK(strstr(log, "step 1 false false\nstep 2 false false\nstep 3 false false\nstep 4 true false\n") != NULL);
    {
        int moves = 0;
        const char *p = g_events;
        while ((p = strstr(p, "ssaction_MoveToPoint")) != NULL) { moves++; p++; }
        CHECK(moves == 1);
    }
    sh_script_map_end();

    CHECK(start(
        "api = 1\n"
        "local step = 0\n"
        "on.think('ai/brute', function(self)\n"
        "  step = step + 1\n"
        "  if step == 1 then self:move_to({x = 500, y = 0, z = 0}) end\n"
        "  local d, f = self:move_done()\n"
        "  print('m ' .. step .. ' ' .. tostring(d) .. ' ' .. tostring(f))\n"
        "end)\n"));
    tick();
    g_ent[1].move_status = 1; tick();
    g_ent[1].move_status = 7; tick();
    CHECK(strstr(log_of("aaaaaaaa"), "m 1 false false\nm 2 false false\nm 3 true true\n") != NULL);
    sh_script_map_end();
}

static void test_queries(void)
{
    const char *log;
    CHECK(start(
        "api = 1\n"
        "local step = 0\n"
        "on.think('ai/brute', function(self)\n"
        "  step = step + 1\n"
        "  if step == 1 then\n"
        "    print('enemy ' .. tostring(self:enemy()))\n"
        "    print('aware ' .. tostring(self:aware()) .. ' ' .. tostring(self:awareness()))\n"
        "  else\n"
        "    local e = self:enemy()\n"
        "    print('same ' .. tostring(e == ent(2)))\n"
        "    print('see ' .. tostring(self:can_see(e)))\n"
        "    print('aware ' .. tostring(self:aware()) .. ' ' .. self:awareness() .. ' ' .. self:awareness(ent(4)))\n"
        "    print(string.format('angle %.1f', self:angle_to(e)))\n"
        "    print('path ' .. tostring(self:path_length({x = 30, y = 40, z = 0})) .. ' ' .. tostring(self:can_reach(e)))\n"
        "    print('path2 ' .. tostring(self:path_length(e)))\n"
        "    local p, why = self:find_position({ min = 100, max = 300, ideal = 200, count = 4 })\n"
        "    print('pos ' .. (p and string.format('%.0f %.0f', p.x, p.y) or why))\n"
        "    print('trigger ' .. tostring(self:trigger_pulled()))\n"
        "  end\n"
        "end)\n"));
    tick();
    g_ent[1].enemy = 2; g_ent[1].visible = 1; g_ent[1].awareness = 3; g_ent[1].path_ready = 1; g_ent[1].trigger = 1;
    tick();
    log = log_of("aaaaaaaa");
    CHECK(strstr(log, "enemy nil\naware false nil\n") != NULL);
    CHECK(strstr(log, "same true\nsee true\naware true confirmed confirmed\n") != NULL);
    CHECK(strstr(log, "angle 45.0\n") != NULL);
    CHECK(strstr(log, "path nil true\n") != NULL);       /* searching on the first ask, found on the second */
    CHECK(strstr(log, "path2 200.0\n") != NULL);
    CHECK(strstr(log, "pos ") != NULL && strstr(log, "pos none") == NULL && strstr(log, "pos searching") == NULL);
    CHECK(strstr(log, "trigger true\n") != NULL);
    CHECK(sh_script_status("aaaaaaaa") == SH_SCRIPT_RUNNING);
    sh_script_map_end();

    /* no path anywhere */
    CHECK(start(
        "api = 1\n"
        "on.think('ai/brute', function(self)\n"
        "  local n, why = self:path_length({x = 1, y = 1, z = 1})\n"
        "  print('nopath ' .. tostring(n) .. ' ' .. why .. ' ' .. tostring(self:can_reach({x = 1, y = 1, z = 1})))\n"
        "  print('fp ' .. select(2, self:find_position({ target = {x = 0, y = 0, z = 0} })))\n"
        "end)\n"));
    g_ent[1].path_none = 1;
    tick();
    CHECK(strstr(log_of("aaaaaaaa"), "nopath nil no path false\nfp none\n") != NULL);
    sh_script_map_end();
}

static void expect_stop(const char *body, const char *phrase)
{
    char script[1024];
    snprintf(script, sizeof script, "api = 1\non.think('ai/brute', function(self)\n%s\nend)\n", body);
    CHECK(start(script));
    tick();
    CHECK(sh_script_status("aaaaaaaa") == SH_SCRIPT_STOPPED);
    if (!strstr(stop_of("aaaaaaaa"), phrase)) printf("stop line: %s\n", stop_of("aaaaaaaa"));
    CHECK(strstr(stop_of("aaaaaaaa"), phrase) != NULL);
    CHECK(strstr(stop_of("aaaaaaaa"), "scripts\\main.lua line 3") != NULL);
    CHECK(strstr(stop_of("aaaaaaaa"), "attempt to") == NULL);
    CHECK(strstr(g_events, "park") == NULL);   /* nothing reached the engine */
    sh_script_map_end();
}

static void test_refusals(void)
{
    expect_stop("ent(4):move_to({x = 1, y = 2, z = 3})", "\"move_to\" works only on this mod's own demons");
    expect_stop("ent(3):move_to({x = 1, y = 2, z = 3})", "\"move_to\" works only on a demon, and this entity is not one");
    expect_stop("self:move_to(nil)", "\"move_to\" needs a position (a table with x, y and z), got nothing");
    expect_stop("self:move_to({x = 1, y = 'a', z = 3})", "\"move_to\" needs a position with a number for y");
    expect_stop("self:move_to({x = 1, y = 2, z = 3}, 5, { fly = true })",
                "\"move_to\" knows the options run, strafe and aim_while_moving, not \"fly\"");
    expect_stop("self:face(42)", "\"face\" needs a position");
    expect_stop("self:equip(-1)", "\"equip\" needs an inventory index from 0 to 63");
    expect_stop("self:laser('a b', true)", "\"laser\": \"a b\" is not a usable name");
    expect_stop("self:find_position({ min = 10, max = 5 })", "\"find_position\": max must not be less than min");
    expect_stop("ent(2):enemy()", "\"enemy\" works only on this mod's own demons");
}

static void test_turn_done(void)
{
    CHECK(start(
        "api = 1\n"
        "local step = 0\n"
        "on.think('ai/brute', function(self)\n"
        "  step = step + 1\n"
        "  if step == 1 then print('t0 ' .. tostring(self:turn_done())) self:face(ent(2)) end\n"
        "  print('t' .. step .. ' ' .. tostring(self:turn_done()))\n"
        "  if step == 3 then self:face({x = 0, y = -50, z = 0}) print('tp ' .. tostring(self:turn_done())) end\n"
        "end)\n"));
    tick();                          /* facing 0, the player at 45 degrees */
    g_ent[1].yaw = 40; tick();       /* within 10 degrees */
    g_ent[1].yaw = -90; tick();      /* then a position straight to its right */
    CHECK(strstr(log_of("aaaaaaaa"), "t0 true\nt1 false\nt2 true\nt3 false\ntp true\n") != NULL);
    sh_script_map_end();
}

static void test_removed_and_unavailable(void)
{
    CHECK(start(
        "api = 1\n"
        "local kept\n"
        "on.think('ai/brute', function(self)\n"
        "  if not kept then kept = self return end\n"
        "end)\n"
        "on.think('prop/rock', function(self)\n"
        "  if kept then print('removed ' .. tostring(kept:move_to({x = 1, y = 1, z = 1})) .. ' ' .. tostring(kept:enemy())) end\n"
        "end)\n"));
    tick();
    g_ent[1].live = 0;
    tick();
    tick();
    CHECK(strstr(log_of("aaaaaaaa"), "removed false nil") != NULL);
    CHECK(sh_script_status("aaaaaaaa") == SH_SCRIPT_RUNNING);
    sh_script_map_end();

    sh_script_ai_set_engine(NULL);
    CHECK(start("api = 1\non.think('ai/brute', function(self) self:stop() end)\n"));
    tick();
    CHECK(strstr(stop_of("aaaaaaaa"), "\"stop\" is not available in this session") != NULL);
    sh_script_map_end();
    sh_script_ai_set_engine(&g_fake);
}

/* test only: ai_anim(self, node, [via], [blend_ms]) is what the entity calls
 * anim_state and anim_force do for a demon (sh_script_ai_anim). */
static int t_ai_anim(lua_State *L)
{
    uint64_t h = sh_script_check_entity(L, 1, "ai_anim");
    const char *node = luaL_checkstring(L, 2);
    const char *via = lua_isnoneornil(L, 3) ? NULL : luaL_checkstring(L, 3);
    int force = !lua_isnoneornil(L, 4);
    lua_pushboolean(L, sh_script_ai_anim(g_fake.entity((sh_script_handle)h), (sh_script_handle)h, node, via, force,
                                         force ? (int)luaL_checkinteger(L, 4) : 0));
    return 1;
}

int main(int argc, char **argv)
{
    sh_script_api_add(SH_SCRIPT_API_GLOBAL, "ent", t_ent);
    sh_script_api_add(SH_SCRIPT_API_GLOBAL, "ai_anim", t_ai_anim);
    CHECK(sh_script_ai_register() == 23);
    sh_script_ai_set_engine(&g_fake);
    sh_script_handles_set_resolver(fake_resolve);

    test_catalogue(argc > 1 ? argv[1] : "fixtures\\scripts\\event_registry_13954591.txt");
    test_args();
    test_move_state();
    test_geometry();
    test_commands();
    test_move_done_and_follow();
    test_queries();
    test_refusals();
    test_turn_done();
    test_removed_and_unavailable();
    files_clear();
    CHECK(sh_script_bytes_in_use() == 0);
    printf(g_fail ? "script_api_ai_test: FAILED\n" : "script_api_ai_test: ok\n");
    return g_fail;
}
