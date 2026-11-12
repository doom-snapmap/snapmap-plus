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
 * stop lines, and a removed demon answering nil.
 *
 * The calls the Tyrant's brain needs: recoil, weapon_ready, aim_point,
 * anim_valid, anim_force_via, firing_position, path_points, spline_clear,
 * monster_type, pain_string, block_pain through scripts; their compositions
 * (sh_ai_firing_run, sh_ai_spline_run, sh_ai_anim_valid_run) over fake
 * primitives with a wall in the world; can_see and find_position against
 * that wall (the trace half of the sight); the pain-string stub run on a
 * synthetic pain request; the per-demon pain records.
 *
 * The engine's fire control calls (fire_mode, pull_trigger, release_trigger,
 * muzzle_on_target, focus, hold, tracking, body_rotation): the engine traffic
 * each one sends (the events, in order, with their arguments), what each reads
 * back from the engine's state, every refusal sentence, and the calls an
 * engine without the game parts refuses plainly. */
#include "heap_out.h"
#include <windows.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "script/script_runtime.h"
#include "script/script_hooks.h"
#include "script/script_api_ai.h"
#include "script/script_api_common.h"
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
    int   path_long;       /* path_points answers this many points (0: three) */
    /* the calls the brain needs */
    float recoil;
    int   ready;           /* weapon_ready answer (-1 no weapon) */
    float aim[3];
    int   aimed;
    char  pain[512];
    int   block_reaction, block_ms;
    uint32_t monster;      /* getMonsterType */
    char  node[96];        /* the animWeb node playing */
    float vel[3];
    /* the weapon: the hand the behaviour fires from (0: the engine's default,
     * the right hand), the inventory and the hand each read asked */
    int   weapon_hand;
    int   nitems;
    const char *item[4];
    int   item_hand[4];
    int   asked_hand;
    /* the engine's fire control and aim focus (the fire calls) */
    int   sim;             /* the events change this state as the engine would */
    int   fire_state, fire_anim;
    int   focus_valid;
    float focus_point[3], focus_radius;
    int   precision, require, tracking, body_rot;
    int   muzzle;          /* the engine's muzzle test answer (-1: unavailable) */
    int   muzzle_mode, muzzle_hand;
    int   gate;            /* the fire gate passes for a checked pull */
    int   clear_moves;
    unsigned no_turn_entry; /* bit per idle turn type: no turnParms entry */
    /* a step of the engine's: the animFSM's stage and the command it waits on */
    int   moving, stage, waiting, idle_turn_on, idle_turn;
    int   interrupts;
} fent;

/* The world: a wall across x = g_wall_x (0: none) up to g_wall_top, and no
 * ground past x = g_hole_x (0: ground everywhere at z 0). */
static float g_wall_x, g_wall_top = 1000.0f, g_hole_x;

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
    if (ev == SH_AI_EV_GET_MONSTER_TYPE) { if (ret) *ret = ((fent *)e)->monster; return 1; }
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
        case 's': case '1': ev_cat(i ? "," : ""); ev_cat(a->s); continue;
        case 'e': snprintf(line, sizeof line, "e%d", idx_of(a->ent)); break;
        case '4': snprintf(line, sizeof line, "noanim"); break;
        default: snprintf(line, sizeof line, "?");
        }
        ev_cat(i ? "," : "");
        ev_cat(line);
    }
    ev_cat(")\n");
    if (((fent *)e)->sim) {
        fent *f = (fent *)e;
        switch (ev) {
        case SH_AI_EV_SET_FIRE_MODE: f->fire_state = args[0].i; break;
        case SH_AI_EV_PULL_TRIGGER: case SH_AI_EV_PULL_TRIGGER_LEFT: f->trigger = 1; break;
        case SH_AI_EV_RELEASE_TRIGGER: case SH_AI_EV_RELEASE_TRIGGER_LEFT: f->trigger = 0; break;
        case SH_AI_EV_AE_PULL_TRIGGER: f->trigger = args[2].i ? f->gate : 1; break;
        case SH_AI_EV_ENABLE_HEAD_TRACKING: f->tracking = args[0].i; break;
        case SH_AI_EV_ENABLE_BODY_ROTATION: f->body_rot = args[0].i; break;
        case SH_AI_EV_CLEAR_AIM_FOCUS: f->focus_valid = 0; break;
        case SH_AI_EV_SET_FOCUS: f->focus_valid = 1; break;
        default: break;
        }
    }
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
static int f_trigger(void *e, int hand) { ((fent *)e)->asked_hand = hand; return ((fent *)e)->trigger; }
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

static int f_eye(void *ai, float out[3]) { memcpy(out, ((fent *)ai)->origin, 12); out[2] += 80; return 1; }
static int f_sight(void *ai, const float a[3], const float b[3], void *target)
{
    (void)ai; (void)target;
    if (g_wall_x != 0 && (a[0] - g_wall_x) * (b[0] - g_wall_x) < 0) {
        float t = (g_wall_x - a[0]) / (b[0] - a[0]);
        if (a[2] + (b[2] - a[2]) * t < g_wall_top) return 0;
    }
    return 1;
}
static int f_ground(const float pos[3], float out[3])
{
    if (g_hole_x != 0 && pos[0] > g_hole_x) return 0;
    out[0] = pos[0]; out[1] = pos[1]; out[2] = 0;
    return 1;
}
/* node "evade/side": two clips, 200 inches left and right; "evade/still": no clip */
static int f_anim_delta(void *ai, const char *node, int index, float out[3], int *count)
{
    (void)ai;
    if (!_stricmp(node, "evade/side")) {
        *count = 2;
        if (index < 0 || index > 1) return 0;
        out[0] = 0; out[1] = index ? -200.0f : 200.0f; out[2] = 0;
        return 1;
    }
    if (!_stricmp(node, "evade/still")) { *count = 0; return 0; }
    *count = 0;
    return -1;
}
static int f_set_recoil(void *ai, float a) { ((fent *)ai)->recoil = a; return 1; }
static int f_weapon_ready(void *ai, int hand) { ((fent *)ai)->asked_hand = hand; return ((fent *)ai)->ready; }
static int f_weapon_hand(void *ai) { return ((fent *)ai)->weapon_hand ? ((fent *)ai)->weapon_hand : SH_AI_HAND_RIGHT; }
static int f_item(void *ai, int index, char *decl, size_t cap, int *hand)
{
    fent *e = (fent *)ai;
    if (index >= e->nitems) return 0;
    strcpy_s(decl, cap, e->item[index]);
    *hand = e->item_hand[index];
    return 1;
}
static int f_aim_point(void *ai, const float p[3]) { memcpy(((fent *)ai)->aim, p, 12); ((fent *)ai)->aimed = 1; return 1; }
static int f_fire_modes(void *ai, int *state, int *anim)
{
    *state = ((fent *)ai)->fire_state; *anim = ((fent *)ai)->fire_anim;
    return 1;
}
static int f_focus_state(void *ai, sh_ai_focus_state *out)
{
    fent *f = (fent *)ai;
    out->valid = f->focus_valid;
    memcpy(out->point, f->focus_point, 12);
    out->radius = f->focus_radius;
    out->precision = f->precision;
    out->require_muzzle = f->require;
    out->tracking = f->tracking;
    out->body_rotation = f->body_rot;
    return 1;
}
static int f_muzzle_test(void *ai, int hand, int mode)
{
    fent *f = (fent *)ai;
    f->muzzle_hand = hand; f->muzzle_mode = mode;
    return f->muzzle;
}
static int f_clear_move(void *ai) { ((fent *)ai)->clear_moves++; ev_cat("clear_move\n"); return 1; }
static int f_idle_turn_entry(void *ai, int turn) { return !(((fent *)ai)->no_turn_entry & (1u << turn)); }
/* the laser targeter and the rear footsteps: the engine side is asked with the tag and the whole decl names */
static int g_no_targeter, g_no_ribbon, g_no_table;
static int f_laser_add(void *ai, const char *tag, const char *ribbon)
{
    char line[256];
    (void)ai;
    if (g_no_targeter) return 0;
    if (ribbon && g_no_ribbon) return -2;
    snprintf(line, sizeof line, "laser_add %s %s\n", tag, ribbon ? ribbon : "(decl)");
    ev_cat(line);
    return 1;
}
static int f_laser_remove(void *ai, const char *tag)
{
    char line[160];
    (void)ai;
    if (g_no_targeter) return 0;
    snprintf(line, sizeof line, "laser_remove %s\n", tag);
    ev_cat(line);
    return 1;
}
static int f_rear_footsteps(void *ai, const char *front, const char *rear)
{
    char line[400];
    (void)ai;
    if (g_no_table) return -2;
    snprintf(line, sizeof line, "rear_footsteps %s -> %s\n", front, rear);
    ev_cat(line);
    return 1;
}
static int g_no_wound, g_no_mesh;
static int f_wound_hides(void *ai, const char *wound, const char *const *meshes, int n)
{
    char line[600];
    int i, used;
    (void)ai;
    if (g_no_wound) return -2;
    if (g_no_mesh) return -3;
    used = snprintf(line, sizeof line, "wound_hides %s:", wound);
    for (i = 0; i < n && used > 0 && used < (int)sizeof line; i++)
        used += snprintf(line + used, sizeof line - (size_t)used, " %s", meshes[i]);
    if (used > 0 && used < (int)sizeof line - 1) { line[used] = '\n'; line[used + 1] = 0; }
    ev_cat(line);
    return 1;
}
static const sh_script_ai_engine g_fake;
static int f_anim_valid(void *ai, const char *node, unsigned tests, int side, int *index)
{
    return sh_ai_anim_valid_run(&g_fake, ai, node, tests, side, 100, index);
}
static int f_path_points(void *ai, const float goal[3], int key, float (**outp)[3], int *n)
{
    fent *f = (fent *)ai;
    float (*out)[3];
    int k, count = f->path_long ? f->path_long : 3;
    (void)key;
    *outp = NULL;
    if (f->path_none) return 2;
    if (++f->path_calls <= f->path_ready) return 1;
    out = (float (*)[3])malloc((size_t)count * sizeof *out);   /* as the engine side: the whole path, malloc'd */
    if (!out) return -1;
    if (f->path_long) {
        for (k = 0; k < count; k++) { out[k][0] = (float)k; out[k][1] = 0; out[k][2] = 0; }
    } else {
        memcpy(out[0], f->origin, 12);
        out[1][0] = goal[0]; out[1][1] = f->origin[1]; out[1][2] = 0;
        memcpy(out[2], goal, 12);
    }
    *outp = out;
    *n = count;
    return 0;
}
static int f_firing(void *ai, const sh_ai_position_query *q, float out[3]) { return sh_ai_firing_run(&g_fake, ai, q, 200, out); }
static int f_spline(void *ai, const sh_ai_spline_query *q, float (*out)[3], int cap, int *n)
{
    return sh_ai_spline_run(&g_fake, ai, q, out, cap, n);
}
static int f_pain_string(void *ai, const char *t) { strcpy(((fent *)ai)->pain, t ? t : "(engine)"); return 1; }
static int f_block_pain(void *ai, int r, int ms) { ((fent *)ai)->block_reaction = r; ((fent *)ai)->block_ms = ms; return 1; }

static int f_step_state(void *ai, sh_ai_step_state *out)
{
    fent *f = (fent *)ai;
    out->moving = f->moving; out->stage = f->stage; out->waiting = f->waiting;
    out->idle_turn = f->idle_turn_on ? f->idle_turn : -1;
    return 1;
}
static int f_step_interrupt(void *ai)
{
    fent *f = (fent *)ai;
    int was = f->waiting;
    f->interrupts++; f->waiting = 0;
    ev_cat("step_interrupt\n");
    return was;
}

static int g_gore_calls;
static int f_gore_init(void *ai)
{
    (void)ai;
    g_gore_calls++;
    return 1;
}

static const sh_script_ai_engine g_fake = {
    f_entity, f_handle_of, f_is_ai, f_origin, f_yaw, f_event, f_park, f_enemy, f_awareness,
    f_move_status, f_trigger, f_path, f_frame,
    f_eye, f_sight, f_ground, f_anim_delta,
    f_set_recoil, f_weapon_ready, f_aim_point, f_anim_valid, f_path_points, f_firing, f_spline,
    f_pain_string, f_block_pain, f_weapon_hand, f_item,
    f_fire_modes, f_focus_state, f_muzzle_test, f_clear_move, f_idle_turn_entry,
    f_laser_add, f_laser_remove, f_rear_footsteps,
    f_step_state, f_step_interrupt, f_gore_init, f_wound_hides
};

/* The entity side of the engine the AI calls also use: the node playing
 * (anim_force_via) and velocity (spline_clear's prediction). */
static void *c_resolve(uint32_t id) { return id < NENT && g_ent[id].live ? &g_ent[id] : NULL; }
static uint32_t c_spawn_id(void *e) { return e ? (uint32_t)idx_of(e) : SH_SCRIPT_SPAWN_ID_NONE; }
static int c_kind(void *e) { return ((fent *)e)->is_ai ? SH_SCRIPT_KIND_AI : SH_SCRIPT_KIND_OTHER; }
static int c_origin(void *e, float out[3]) { memcpy(out, ((fent *)e)->origin, 12); return 1; }
static int c_event(void *e, const char *name, const sh_script_earg *args, int n, sh_script_earg *ret, int post)
{
    (void)args; (void)n; (void)post;
    if (ret) memset(ret, 0, sizeof *ret);
    if (ret && !strcmp(name, "getLinearVelocity")) { ret->type = 'v'; memcpy(ret->v, ((fent *)e)->vel, 12); }
    return 1;
}
static char *c_anim_node(void *e)
{
    if (!((fent *)e)->node[0]) return NULL;
    return _strdup(((fent *)e)->node);
}
/* the demon's md6Def tags: its aim tags only */
static int c_tag(void *e, const char *name, float pos[3], float axis[9])
{
    (void)axis;
    if (!((fent *)e)->is_ai || strncmp(name, "focus_aim", 9)) return 0;
    memcpy(pos, ((fent *)e)->origin, 12);
    return 1;
}
static const sh_script_engine g_common = {
    c_resolve, c_spawn_id, NULL, NULL, c_kind, c_origin, NULL, NULL, NULL, c_event,
    NULL, NULL, NULL, c_anim_node, NULL, NULL, NULL, f_ground, NULL, c_tag
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
static char g_stop[4096];
static const char *stop_of(const char *salt)
{
    HOS(g_stop, sh_script_stop_line_dup(salt));
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
    g_wall_x = 0; g_wall_top = 1000.0f; g_hole_x = 0;
    sh_script_api_anim_reset();
    memset(g_ent, 0, sizeof g_ent);
    g_events[0] = 0;
    g_frame = 0;
    g_ent[1].live = 1; g_ent[1].is_ai = 1;
    g_ent[1].nitems = 3;   /* claw (right hand), cannon (left), mortar (left) */
    g_ent[1].item[0] = "mods/aaaaaaaa/weapon/ai/brute/claw";
    g_ent[1].item_hand[0] = SH_AI_HAND_RIGHT;
    g_ent[1].item[1] = "mods/aaaaaaaa/weapon/ai/brute/cannon";
    g_ent[1].item_hand[1] = SH_AI_HAND_LEFT;
    g_ent[1].item[2] = "mods/aaaaaaaa/weapon/ai/brute/mortar";
    g_ent[1].item_hand[2] = SH_AI_HAND_LEFT;
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
    sh_script_api_frame();
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
    /* a forced via: wait for the node, ask for the destination once it plays, done when the destination plays */
    CHECK(sh_ai_force_via_next(0, 0, 10, 600) == SH_AI_FORCE_VIA_WAIT);
    CHECK(sh_ai_force_via_next(0, 0, 601, 600) == SH_AI_FORCE_VIA_DONE);            /* never seen: given up */
    CHECK(sh_ai_force_via_next(SH_SCRIPT_ANIM_VIA_IN, 0, 3, 600) == SH_AI_FORCE_VIA_ASK_DEST);
    CHECK(sh_ai_force_via_next(SH_SCRIPT_ANIM_VIA_IN, 1, 5000, 600) == SH_AI_FORCE_VIA_WAIT);   /* a long clip is waited for */
    CHECK(sh_ai_force_via_next(SH_SCRIPT_ANIM_VIA_IN | SH_SCRIPT_ANIM_VIA_OUT, 0, 9, 600) == SH_AI_FORCE_VIA_ASK_DEST);
    CHECK(sh_ai_force_via_next(SH_SCRIPT_ANIM_VIA_IN | SH_SCRIPT_ANIM_DEST, 1, 9, 600) == SH_AI_FORCE_VIA_DONE);
    /* a parked demon stays the script's except while dead, or while a pain reaction is asked for or playing */
    CHECK(sh_ai_park_holds(1, 0, 0) == 1);
    CHECK(sh_ai_park_holds(0, 0, 0) == 0);          /* dead: the engine's dying cycle takes it */
    CHECK(sh_ai_park_holds(1, 1, 0) == 0);          /* this frame's damage asked for a pain */
    CHECK(sh_ai_park_holds(1, 0, 6) == 0);          /* PAIN_FALTER is playing */
    CHECK(sh_ai_park_holds(1, 0, 9) == 0);          /* PAIN_STAGGER is playing */
    CHECK(sh_ai_park_holds(0, 1, 6) == 0);
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
        "    self:trigger(true, 'left')\n"
        "    self:trigger(false, 'left')\n"
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
    CHECK(strstr(g_events, "1:pullTriggerRight()\n1:releaseTriggerRight()\n1:pullTriggerLeft()\n"
                           "1:releaseTriggerLeft()\n1:equipInventoryItemByIndex(2)\n") != NULL);
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
    expect_stop("ent(3):move_to({x = 1, y = 2, z = 3})", "\"move_to\" works only on a demon, and this entity is not one");
    expect_stop("self:move_to(nil)", "\"move_to\" needs a position (a table with x, y and z), got nothing");
    expect_stop("self:move_to({x = 1, y = 'a', z = 3})", "\"move_to\" needs a position with a number for y");
    expect_stop("self:move_to({x = 1, y = 2, z = 3}, 5, { fly = true })",
                "\"move_to\" knows the options run, strafe and aim_while_moving, not \"fly\"");
    expect_stop("self:face(42)", "\"face\" needs a position");
    expect_stop("self:focus_tag('arm_hand_lf')", "\"focus_tag\": \"arm_hand_lf\" is not a tag of this demon's md6Def "
                "(the game aims only at tags, not joints)");
    expect_stop("self:equip(-1)", "\"equip\" needs an inventory index of 0 or more, got -1");
    expect_stop("self:laser('a b', true)", "\"laser\": \"a b\" is not a usable name");
    expect_stop("self:find_position({ min = 10, max = 5 })", "\"find_position\": max must not be less than min");
}

/* A tag, laser or pain string name is any length the decl gives it: it reaches the engine whole. */
static void test_long_tag_names(void)
{
    char want[600];
    int i;
    CHECK(start(
        "api = 1\n"
        "local done = false\n"
        "on.think('ai/brute', function(self)\n"
        "  if not done then\n"
        "    done = true\n"
        "    self:laser(string.rep('laser_source_', 30), true)\n"
        "    self:laser_sweep(string.rep('sweep', 40))\n"
        "    self:pain_string(string.rep('combo', 50))\n"
        "  end\n"
        "end)\n"));
    tick();
    CHECK(sh_script_status("aaaaaaaa") == SH_SCRIPT_RUNNING);
    strcpy(want, "1:setLaserState(1,");
    for (i = 0; i < 30; i++) strcat(want, "laser_source_");
    strcat(want, ")\n");
    CHECK(strstr(g_events, want) != NULL);
    strcpy(want, "1:ae_startLaserSweep(noanim,");
    for (i = 0; i < 40; i++) strcat(want, "sweep");
    strcat(want, ")\n");
    CHECK(strstr(g_events, want) != NULL);
    want[0] = 0;
    for (i = 0; i < 50; i++) strcat(want, "combo");
    CHECK(strcmp(g_ent[1].pain, want) == 0);
    sh_script_map_end();
    expect_stop("self:laser('', true)", "\"laser\" needs a laser tag");
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

/* ------------------------------------------- the calls the brain needs */

static void test_brain_pure(void)
{
    char bad[64];
    float a[3] = { 0, 0, 0 }, b[3] = { 100, 0, 0 }, o[3], pts[8][3], tt[40], tv[40], many[42][3];
    sh_ai_table tb;
    int n, k;
    printf("-- the brain's calls: words, names, curves\n");
    CHECK(sh_ai_tests_parse("TF_BOUNDS TF_NAV_ENDPOINT", bad, sizeof bad) == (SH_AI_TF_BOUNDS | SH_AI_TF_NAV_ENDPOINT));
    CHECK(sh_ai_tests_parse("VALIDATE_ANIM_DELTA", bad, sizeof bad) == SH_AI_TF_ANIM_DELTA);
    CHECK(sh_ai_tests_parse("TF_BOUNDS TF_FLY", bad, sizeof bad) == 0 && !strcmp(bad, "TF_FLY"));
    CHECK(sh_ai_tests_parse("", bad, sizeof bad) == 0 && bad[0] == 0);
    CHECK(!strcmp(sh_ai_monster_name(0), "AI_MONSTER_NONE"));
    CHECK(!strcmp(sh_ai_monster_name(1), "AI_MONSTER_GENERIC"));
    CHECK(!strcmp(sh_ai_monster_name(65536), "AI_MONSTER_BARON"));
    CHECK(!strcmp(sh_ai_monster_name(4194304), "AI_MONSTER_SPIDER_MASTERMIND"));
    CHECK(sh_ai_monster_name(3) == NULL && sh_ai_monster_name(1u << 23) == NULL);
    CHECK(sh_ai_position_kind("superior") == SH_AI_POS_SUPERIOR && sh_ai_position_kind(NULL) == SH_AI_POS_ANY);
    CHECK(sh_ai_position_kind("best") == -1);
    sh_ai_line_offset(a, b, 0.5f, 10, 90, o);           /* to the left of +x is +y */
    CHECK(fabsf(o[0] - 50) < 1e-3f && fabsf(o[1] - 10) < 1e-3f && fabsf(o[2]) < 1e-3f);
    sh_ai_line_offset(a, b, 0.5f, 10, -90, o);
    CHECK(fabsf(o[1] + 10) < 1e-3f);
    sh_ai_line_offset(a, b, 0.25f, 10, 0, o);           /* roll 0: up */
    CHECK(fabsf(o[0] - 25) < 1e-3f && fabsf(o[2] - 10) < 1e-3f);
    memset(&tb, 0, sizeof tb);
    tb.t = tt; tb.v = tv;
    tb.n = 3; tb.t[0] = 0.25f; tb.v[0] = 0.2f; tb.t[1] = 0.5f; tb.v[1] = 0.4f; tb.t[2] = 1.0f; tb.v[2] = 0;
    CHECK(fabsf(sh_ai_table_at(&tb, 0.125f) - 0.1f) < 1e-5f);   /* from 0 at the start */
    CHECK(fabsf(sh_ai_table_at(&tb, 0.375f) - 0.3f) < 1e-5f);
    CHECK(fabsf(sh_ai_table_at(&tb, 1.0f)) < 1e-5f);
    n = sh_ai_spline_points(a, b, &tb, 90, pts, 8);
    CHECK(n == 4);                                        /* the ends and the two inner knots */
    CHECK(fabsf(pts[1][0] - 25) < 1e-3f && fabsf(pts[1][1] - 20) < 1e-3f);   /* 0.2 x 100 to the left */
    CHECK(fabsf(pts[2][0] - 50) < 1e-3f && fabsf(pts[2][1] - 40) < 1e-3f);
    CHECK(pts[3][0] == 100 && pts[3][1] == 0);
    /* 40 knots (the old table held 16): every one is a point of the curve */
    for (k = 0; k < 40; k++) { tt[k] = (k + 1) / 41.0f; tv[k] = 0.1f; }
    tb.n = 40;
    n = sh_ai_spline_points(a, b, &tb, 90, many, tb.n + 2);
    CHECK(n == 42 && many[41][0] == 100 && fabsf(many[40][1] - 10) < 1e-3f);
}

/* The compositions over the fake primitives: the demon (1) at the origin
 * facing +x, its enemy (2) 600 inches ahead. */
static void test_compositions(void)
{
    sh_ai_position_query q;
    sh_ai_spline_query sq;
    float out[3], pts[20][3], st_t[2], st_v[2];
    int idx = -1, n = 0;
    printf("-- the compositions: anim_valid, firing_position, spline_clear\n");
    world();
    g_ent[1].enemy = 2;
    g_ent[2].origin[0] = 600; g_ent[2].origin[1] = 0;
    g_ent[2].yaw = 180;                                   /* the enemy looks back at the demon */
    /* anim_valid: both side steps fit an open floor; the first one is the answer */
    CHECK(sh_ai_anim_valid_run(&g_fake, &g_ent[1], "evade/side", SH_AI_TF_ANIM_DELTA, -1, 0, &idx) == 1 && idx == 0);
    CHECK(sh_ai_anim_valid_run(&g_fake, &g_ent[1], "evade/side", SH_AI_TF_ANIM_DELTA, 1, 0, &idx) == 1 && idx == 1);
    CHECK(sh_ai_anim_valid_run(&g_fake, &g_ent[1], "evade/still", SH_AI_TF_ANIM_DELTA, -1, 0, &idx) == 0);
    CHECK(sh_ai_anim_valid_run(&g_fake, &g_ent[1], "evade/none", SH_AI_TF_ANIM_DELTA, -1, 0, &idx) == -1);
    /* the enemy's view centre: a side step out of a straight line keeps 18 degrees: refused at 20 */
    CHECK(sh_ai_anim_valid_run(&g_fake, &g_ent[1], "evade/side", SH_AI_TF_AVOID_ENEMY_FOV, -1, 0, &idx) == 0);
    /* the eyes from the end point: a wall between the step and the enemy */
    g_wall_x = 300;
    CHECK(sh_ai_anim_valid_run(&g_fake, &g_ent[1], "evade/side", SH_AI_TF_EYE_VISIBILITY, -1, 0, &idx) == 0);
    g_wall_x = 0;
    /* a path still being searched answers 2, then the step fits */
    g_ent[1].path_ready = 1; g_ent[1].path_calls = 0;
    CHECK(sh_ai_anim_valid_run(&g_fake, &g_ent[1], "evade/side", SH_AI_TF_NAV_DIRECT, 0, 0, &idx) == 2);
    CHECK(sh_ai_anim_valid_run(&g_fake, &g_ent[1], "evade/side", SH_AI_TF_NAV_DIRECT, 0, 0, &idx) == 1);
    /* no ground where the step ends */
    g_hole_x = -1;
    CHECK(sh_ai_anim_valid_run(&g_fake, &g_ent[1], "evade/side", SH_AI_TF_NAV_ENDPOINT, -1, 0, &idx) == 0);
    g_hole_x = 0;

    /* firing_position: the current place (600 from the target, ideal 600, seeing it) is kept for "any";
     * "new" moves; "superior" finds nothing better; a wall at x 300 makes every place on the near side blind */
    memset(&q, 0, sizeof q);
    q.target = &g_ent[2];
    q.min = 300; q.max = 900; q.ideal = 600; q.visible = 1; q.count = 8; q.path_weight = 0.5f;
    q.kind = SH_AI_POS_ANY;
    CHECK(sh_ai_firing_run(&g_fake, &g_ent[1], &q, 0, out) == 0 && out[0] == 0 && out[1] == 0);
    q.kind = SH_AI_POS_NEW;
    CHECK(sh_ai_firing_run(&g_fake, &g_ent[1], &q, 0, out) == 0 && (out[0] != 0 || out[1] != 0));
    q.kind = SH_AI_POS_SUPERIOR;
    CHECK(sh_ai_firing_run(&g_fake, &g_ent[1], &q, 0, out) == 2);
    g_wall_x = 300;   /* the demon is blind now: any place that sees over the wall is superior */
    CHECK(sh_ai_firing_run(&g_fake, &g_ent[1], &q, 0, out) == 0 && out[0] > 300);
    q.kind = SH_AI_POS_REPOSITION;
    CHECK(sh_ai_firing_run(&g_fake, &g_ent[1], &q, 0, out) == 0 && out[0] > 600);   /* the far half */
    q.count = 500;                                        /* the old ring held 16 candidates */
    g_ent[1].path_calls = 0;
    CHECK(sh_ai_firing_run(&g_fake, &g_ent[1], &q, 0, out) == 0 && out[0] > 599.9f);
    CHECK(g_ent[1].path_calls > 16);                      /* every candidate past the old 16 was asked */
    q.count = 8;
    g_wall_x = 0;
    g_ent[1].path_none = 1;
    q.kind = SH_AI_POS_NEW;
    CHECK(sh_ai_firing_run(&g_fake, &g_ent[1], &q, 0, out) == 2);
    g_ent[1].path_none = 0;

    /* spline_clear: open ground is clear, with the points on it; a wall or a hole is not */
    memset(&sq, 0, sizeof sq);
    sq.target = &g_ent[2];
    memcpy(sq.to, g_ent[2].origin, 12);
    sq.table.t = st_t; sq.table.v = st_v;
    sq.table.n = 2; sq.table.t[0] = 0.5f; sq.table.v[0] = 0.25f; sq.table.t[1] = 1.0f; sq.table.v[1] = 0;
    sq.roll = -90; sq.nav_ground = 1;
    CHECK(sh_ai_spline_run(&g_fake, &g_ent[1], &sq, pts, 20, &n) == 1 && n == 3);
    CHECK(fabsf(pts[1][0] - 300) < 1e-3f && fabsf(pts[1][1] + 150) < 1e-3f);   /* 0.25 x 600 to the right */
    g_wall_x = 250;
    CHECK(sh_ai_spline_run(&g_fake, &g_ent[1], &sq, pts, 20, &n) == 0);
    g_wall_x = 0;
    g_hole_x = 200;
    CHECK(sh_ai_spline_run(&g_fake, &g_ent[1], &sq, pts, 20, &n) == 0);
    sq.nav_ground = 0;
    CHECK(sh_ai_spline_run(&g_fake, &g_ent[1], &sq, pts, 20, &n) == 1);   /* without TF_NAV_GROUND the hole is fine */
    g_hole_x = 0;
}

/* can_see and find_position against an occluder: the trace path they use
 * (the sight primitive from the eye) answers false behind a wall even while
 * the perception says it has seen the player. */
static void test_sight_against_a_wall(void)
{
    const char *log;
    printf("-- can_see and find_position against a wall\n");
    CHECK(start(
        "api = 1\n"
        "on.think('ai/brute', function(self)\n"
        "  print('see ' .. tostring(self:can_see(ent(2))))\n"
        "  local p, why = self:find_position({ target = ent(2), min = 100, max = 300, ideal = 200, count = 8 })\n"
        "  print('pos ' .. (p and string.format('%.0f %.0f', p.x, p.y) or why))\n"
        "  local q = self:find_position({ target = ent(2), ideal = 200, min = 100, max = 300, count = 8, visible = false })\n"
        "  print('blind ' .. tostring(q ~= nil))\n"
        "end)\n"));
    g_ent[1].visible = 1;
    g_ent[2].origin[0] = 600; g_ent[2].origin[1] = 0;
    tick();                                  /* open room: seen, a place found */
    g_wall_x = 300; g_wall_top = 1000;
    tick();                                  /* a wall between them, perception still remembers */
    g_ent[1].visible = 0; g_wall_x = 0;
    tick();                                  /* no wall, perception has not seen it: not seen */
    log = log_of("aaaaaaaa");
    CHECK(strstr(log, "see true\npos ") != NULL);
    CHECK(strstr(log, "see false\npos ") != NULL);
    /* with the wall, only the places beyond it (x > 300) see the player */
    {
        const char *second = strstr(log, "see false\npos ");
        float x = 0, y = 0;
        CHECK(second && sscanf(second + 14, "%f %f", &x, &y) == 2 && x > 300);
    }
    CHECK(strstr(log, "blind true") != NULL);
    sh_script_map_end();
}

static void test_brain_calls(void)
{
    const char *log;
    printf("-- the brain's calls through scripts\n");
    CHECK(start(
        "api = 1\n"
        "local step = 0\n"
        "on.think('ai/brute', function(self)\n"
        "  step = step + 1\n"
        "  if step == 1 then\n"
        "    self:recoil(0.2)\n"
        "    print('ready ' .. tostring(self:weapon_ready()))\n"
        "    self:aim_point({x = 1, y = 2, z = 3})\n"
        "    print('valid ' .. tostring(self:anim_valid('evade/side', 'TF_NAV_ENDPOINT TF_BOUNDS', 1)))\n"
        "    print('valid2 ' .. tostring(self:anim_valid('evade/side')))\n"
        "    print('valid3 ' .. tostring(self:anim_valid('evade/still', 'VALIDATE_ANIM_DELTA')))\n"
        "    print('valid4 ' .. table.concat({tostring(select(1, self:anim_valid('evade/none'))), select(2, self:anim_valid('evade/none'))}, ' '))\n"
        "    local p, why = self:path_points({x = 30, y = 40, z = 0})\n"
        "    print('pts ' .. tostring(p) .. ' ' .. tostring(why))\n"
        "    print('mt ' .. tostring(ent(4):monster_type()) .. ' ' .. tostring(ent(3):monster_type()) .. ' ' .. tostring(self:monster_type()))\n"
        "    self:pain_string('combo')\n"
        "    self:block_pain('PAIN_PUSHBACK', 7)\n"
        "  else\n"
        "    local p = self:path_points({x = 30, y = 40, z = 0})\n"
        "    print('pts2 ' .. #p .. ' ' .. p[2].x .. ' ' .. p[3].y)\n"
        "    local f, why = self:firing_position({ target = ent(2), kind = 'new', min = 50, max = 400, ideal = 200, visible = true, parms = {} })\n"
        "    print('fp ' .. tostring(f ~= nil) .. ' ' .. tostring(why))\n"
        "    local g = self:firing_position({ kind = 'superior', min = 50, max = 400, ideal = 141 })\n"
        "    print('fp2 ' .. tostring(g) .. ' ' .. tostring(select(2, self:firing_position({ kind = 'superior', min = 50, max = 400, ideal = 141 }))))\n"
        "    local ok, pts = self:spline_clear(ent(2), { times = {0.5, 1}, values = {0.25}, roll = { value = 90 } }, 1, 'TF_NAV_GROUND')\n"
        "    print('spline ' .. tostring(ok) .. ' ' .. #pts .. string.format(' %.0f %.0f', pts[3].x, pts[3].y))\n"
        "    local ok2 = self:spline_clear(ent(2), { table = { times = { ['times[1]'] = 0.5, ['times[2]'] = 1 }, values = { ['values[1]'] = 0.1 } } }, 0)\n"
        "    print('spline2 ' .. tostring(ok2))\n"
        "    local ok3, p3 = self:spline_clear(ent(2), { table = { {t = 0, v = 0}, {t = 0.5, v = 0.25}, {t = 1, v = 0} }, roll = -90 }, 0)\n"
        "    print(string.format('spline3 %s %d %.0f', tostring(ok3), #p3, p3[2].y))\n"
        "    local knots = {} for i = 1, 60 do knots[i] = {t = i / 61, v = 0.05} end\n"
        "    local ok4, p4 = self:spline_clear(ent(2), { table = knots, roll = 90 }, 0)\n"
        "    local ts, vs = {}, {} for i = 1, 30 do ts[i] = i / 31; vs[i] = 0.02 end ts[31] = 1\n"
        "    local ok5, p5 = self:spline_clear(ent(2), { times = ts, values = vs }, 0)\n"
        "    print('long splines ' .. tostring(ok4) .. ' ' .. #p4 .. ' ' .. tostring(ok5) .. ' ' .. #p5)\n"
        "    local many = self:find_position({ target = ent(2), min = 100, max = 70000, ideal = 200, count = 64, visible = false })\n"
        "    print('many ' .. tostring(many ~= nil))\n"
        "    self:pain_string(nil)\n"
        "    self:aim_point(nil)\n"
        "    print('ready2 ' .. tostring(self:weapon_ready()))\n"
        "  end\n"
        "end)\n"));
    g_ent[1].ready = 1;
    g_ent[1].path_ready = 1;
    g_ent[4].monster = 65536;
    tick();
    g_ent[1].enemy = 2;
    g_ent[1].ready = 0;
    g_ent[2].vel[0] = 10;
    tick();
    log = log_of("aaaaaaaa");
    CHECK(fabsf(g_ent[1].recoil - 0.2f) < 1e-6f);
    CHECK(strstr(log, "ready true\n") != NULL && strstr(log, "ready2 false\n") != NULL);
    CHECK(g_ent[1].aimed && g_ent[1].aim[0] == 1 && g_ent[1].aim[2] == 3);
    CHECK(strstr(log, "valid true\nvalid2 true\nvalid3 false\nvalid4 nil no such node\n") != NULL);
    CHECK(strstr(log, "pts nil searching\n") != NULL);
    CHECK(strstr(log, "mt AI_MONSTER_BARON nil AI_MONSTER_NONE\n") != NULL);
    CHECK(g_ent[1].block_reaction == 7 && g_ent[1].block_ms == 7000);    /* PAIN_PUSHBACK */
    CHECK(!strcmp(g_ent[1].pain, "(engine)"));                            /* combo, then nil */
    CHECK(strstr(log, "pts2 3 30.0 40.0\n") != NULL);
    CHECK(strstr(log, "fp true nil\n") != NULL);
    CHECK(strstr(log, "fp2 nil none\n") != NULL);
    CHECK(strstr(log, "spline3 true 3 ") != NULL);   /* the knot at 0.5 is inside: three points */
    CHECK(strstr(log, "spline true 3 110 100\n") != NULL);                  /* the player one second ahead */
    CHECK(strstr(log, "spline2 true\n") != NULL);
    CHECK(strstr(log, "long splines true 62 true 32\n") != NULL);   /* never cut: every knot inside is a point */
    CHECK(strstr(log, "many true\n") != NULL);                      /* 64 candidates, a max past the old 65,536 */
    CHECK(strstr(g_events, "1:clearAimFocus()\n") != NULL);
    CHECK(sh_script_status("aaaaaaaa") == SH_SCRIPT_RUNNING);
    if (g_fail) printf("brain log:\n%s", log);
    sh_script_map_end();
}

/* A long path: path_points answers every point the engine's path has (the old
 * buffer held 64). */
/* trigger, trigger_pulled, weapon_ready and equip act on the same weapon:
 * the one in the hand a script names, else the demon's weapon hand; equip
 * takes the item by its decl and says where it went, and tells the author
 * once when the game would never fire it. */
static void test_weapon_hands(void)
{
    const char *log;
    printf("-- the weapon calls agree on the hand and the item\n");
    CHECK(start(
        "api = 1\n"
        "local step = 0\n"
        "on.think('ai/brute', function(self)\n"
        "  step = step + 1\n"
        "  if step == 1 then\n"
        "    print('hand ' .. tostring(self:equip('weapon/ai/brute/cannon')))\n"
        "    print('hand2 ' .. tostring(self:equip('weapon/ai/brute/cannon')))\n"
        "    print('hand3 ' .. tostring(self:equip(0)))\n"
        "    self:trigger(true)\n"
        "    print('pulled ' .. tostring(self:trigger_pulled()))\n"
        "    self:trigger(true, 'left')\n"
        "    print('ready ' .. tostring(self:weapon_ready('left')))\n"
        "    self:trigger(false, 'right')\n"
        "    print('full ' .. tostring(self:equip('mods/aaaaaaaa/weapon/ai/brute/claw')))\n"
        "  elseif step == 2 then\n"
        "    self:trigger(true)\n"
        "    print('pulled2 ' .. tostring(self:trigger_pulled()))\n"
        "    print(pcall(self.equip, self, 'weapon/none'))\n"
        "    print(pcall(self.equip, self, 9))\n"
        "    print(pcall(self.trigger, self, true, 'third'))\n"
        "    print(pcall(self.equip, self, -1))\n"
        "  end\n"
        "end)\n"));
    g_ent[1].nitems = 2;   /* this test carries two */
    g_ent[1].item[0] = "mods/aaaaaaaa/weapon/ai/brute/claw";
    g_ent[1].item_hand[0] = SH_AI_HAND_RIGHT;
    g_ent[1].item[1] = "mods/aaaaaaaa/weapon/ai/brute/cannon";
    g_ent[1].item_hand[1] = SH_AI_HAND_LEFT;
    g_ent[1].trigger = 1;
    g_ent[1].ready = 1;
    tick();
    log = log_of("aaaaaaaa");
    CHECK(strstr(log, "hand left\nhand2 left\nhand3 right\n") != NULL);
    /* the right hand is the weapon hand: the cannon in the left one is never fired, said once */
    CHECK(strstr(log, "weapon/ai/brute/cannon goes in the demon's left hand") != NULL);
    CHECK(strstr(log, "set weaponSlot = \"EQUIP_LEFT_HAND\"") != NULL);
    CHECK(strstr(strstr(log, "goes in the demon's left hand") + 1, "goes in the demon's left hand") == NULL);
    CHECK(strstr(g_events, "1:equipInventoryItemByIndex(1)\n1:equipInventoryItemByIndex(1)\n"
                           "1:equipInventoryItemByIndex(0)\n1:pullTriggerRight()\n") != NULL);
    CHECK(strstr(log, "pulled true\n") != NULL);
    CHECK(strstr(g_events, "1:pullTriggerLeft()\n1:releaseTriggerRight()\n") != NULL);
    CHECK(strstr(log, "ready true\n") != NULL && g_ent[1].asked_hand == SH_AI_HAND_LEFT);
    CHECK(strstr(log, "full right\n") != NULL);
    /* a demon whose behaviour fires from the left hand: no hand named means the left one */
    g_ent[1].weapon_hand = SH_AI_HAND_LEFT;
    g_events[0] = 0;
    tick();
    log = log_of("aaaaaaaa");
    CHECK(strstr(g_events, "1:pullTriggerLeft()\n") != NULL && g_ent[1].asked_hand == SH_AI_HAND_LEFT);
    CHECK(strstr(log, "pulled2 true\n") != NULL);
    CHECK(strstr(log, "this demon carries no \"weapon/none\" (it carries: weapon/ai/brute/claw, "
                      "weapon/ai/brute/cannon)") != NULL);
    CHECK(strstr(log, "this demon has no inventory item 9") != NULL);
    CHECK(strstr(log, "\"trigger\" needs the hand as \"left\" or \"right\"") != NULL);
    CHECK(strstr(log, "\"equip\" needs an inventory index of 0 or more, got -1") != NULL);
    sh_script_map_end();
}

/* face: exact by default; { travel = inches } becomes the move's tolerance,
 * the radius the engine's turn-clip query lets a clip's root step. */
static void test_face_travel(void)
{
    printf("-- face with turn clips that travel\n");
    CHECK(start(
        "api = 1\n"
        "on.think('ai/brute', function(self)\n"
        "  self:face({x = 0, y = 100, z = 0})\n"
        "  self:face({x = 0, y = 100, z = 0}, { travel = 40 })\n"
        "  self:face(ent(2), { travel = 30 })\n"
        "  self:face(ent(2))\n"
        "  print(pcall(self.face, self, ent(2), { speed = 3 }))\n"
        "  print(pcall(self.face, self, ent(2), { travel = -1 }))\n"
        "end)\n"));
    tick();
    CHECK(strstr(g_events, "1:ssaction_MoveToPoint(0 0 0,2,0.1,1,0 1 0)\n"
                           "1:ssaction_MoveToPoint(0 0 0,2,40,1,0 1 0)\n"
                           "1:ssaction_MoveToPoint(0 0 0,2,30,1,0.707107 0.707107 0)\n"
                           "1:setFocus(e2,0,1,") != NULL);
    CHECK(strstr(log_of("aaaaaaaa"), "\"face\" knows the option travel, not \"speed\"") != NULL);
    CHECK(strstr(g_log, "\"face\" needs travel as a distance in inches, 0 or more") != NULL);
    sh_script_map_end();
}

static void test_long_path(void)
{
    const char *log;
    printf("-- path_points answers a long path whole\n");
    CHECK(start("api = 1\n"
                "on.think('ai/brute', function(self)\n"
                "  local p = self:path_points({x = 30, y = 40, z = 0})\n"
                "  print('long path ' .. #p .. ' ' .. p[300].x)\n"
                "end)\n"));
    g_ent[1].path_long = 300;
    tick();
    log = log_of("aaaaaaaa");
    CHECK(strstr(log, "long path 300 299.0\n") != NULL);
    g_ent[1].path_long = 0;
    sh_script_map_end();
}

/* The engine's fire control through scripts: what each call sends and reads. */
static void test_fire_calls(void)
{
    const char *log;
    printf("-- the fire control calls send the engine's events and read its state\n");
    CHECK(start(
        "api = 1\n"
        "local step = 0\n"
        "on.think('ai/brute', function(self)\n"
        "  step = step + 1\n"
        "  if step ~= 1 then return end\n"
        "  print('mode0 ' .. table.concat({self:fire_mode()}, ' '))\n"
        "  print('set ' .. tostring(self:fire_mode('FIREWHENREADY')))\n"
        "  print('mode1 ' .. table.concat({self:fire_mode()}, ' '))\n"
        "  self:fire_mode('aifiremode_sprayWhenReady')\n"
        "  self:fire_mode(0)\n"
        "  print('mode2 ' .. table.concat({self:fire_mode()}, ' '))\n"
        "  print('pull ' .. tostring(self:pull_trigger()))\n"
        "  print('pulled ' .. tostring(self:trigger_pulled()))\n"
        "  self:release_trigger()\n"
        "  print('released ' .. tostring(self:trigger_pulled()))\n"
        "  print('gated ' .. tostring(self:pull_trigger(true)))\n"
        "  print('gated left ' .. tostring(self:pull_trigger(true, 'left')))\n"
        "  self:release_trigger('left')\n"
        "  print('plain left ' .. tostring(self:pull_trigger(false, 'left')))\n"
        "  local on, info = self:muzzle_on_target()\n"
        "  print(('muzzle %s %s %s %.1f %s %s %s %s'):format(tostring(on), tostring(info.focus_valid),\n"
        "    info.point.x, info.radius, info.precision, tostring(info.require_muzzle), tostring(info.tracking),\n"
        "    tostring(info.body_rotation)))\n"
        "  print('firing ' .. tostring(self:muzzle_on_target('firing', 'left')))\n"
        "  self:focus(ent(2))\n"
        "  self:focus({x = 1, y = 2, z = 3})\n"
        "  self:focus(nil)\n"
        "  print('hold ' .. tostring(self:hold()))\n"
        "  print('hold2 ' .. tostring(self:hold('RELAXED')))\n"
        "  print('hold3 ' .. table.concat({tostring(self:hold('AIIDLETURN_INCOVER'))}, ' '))\n"
        "  local h4, why4 = self:hold('incover')\n"
        "  print('hold4 ' .. tostring(h4) .. ' | ' .. tostring(why4))\n"
        "  print('track ' .. tostring(self:tracking()) .. ' ' .. tostring(self:body_rotation()))\n"
        "  print('set tracking ' .. tostring(self:tracking(false)) .. ' ' .. tostring(self:body_rotation(true)))\n"
        "  print('track2 ' .. tostring(self:tracking()) .. ' ' .. tostring(self:body_rotation()))\n"
        "end)\n"));
    g_ent[1].sim = 1;
    g_ent[1].ready = 1;
    g_ent[1].gate = 0;
    g_ent[1].focus_valid = 1;
    g_ent[1].focus_point[0] = 12; g_ent[1].focus_point[1] = 3; g_ent[1].focus_point[2] = 4;
    g_ent[1].focus_radius = 24.5f;
    g_ent[1].precision = 1;
    g_ent[1].require = 1;
    g_ent[1].tracking = 1;
    g_ent[1].body_rot = 1;
    g_ent[1].muzzle = 1;
    g_ent[1].no_turn_entry = 1u << SH_AI_IDLETURN_INCOVER;
    tick();
    log = log_of("aaaaaaaa");
    if (g_fail) printf("fire log:\n%s\nevents:\n%s", log, g_events);
    if (sh_script_status("aaaaaaaa") != SH_SCRIPT_RUNNING) printf("stop: %s\nlog:\n%s\n", stop_of("aaaaaaaa"), log);
    CHECK(sh_script_status("aaaaaaaa") == SH_SCRIPT_RUNNING);
    /* the fire mode: read, set by the engine's event, read back (the animation event's mode apart) */
    CHECK(strstr(log, "mode0 AIFIREMODE_HOLDFIRE AIFIREMODE_HOLDFIRE\n") != NULL);
    CHECK(strstr(log, "set true\n") != NULL);
    CHECK(strstr(log, "mode1 AIFIREMODE_FIREWHENREADY AIFIREMODE_HOLDFIRE\n") != NULL);
    CHECK(strstr(log, "mode2 AIFIREMODE_HOLDFIRE AIFIREMODE_HOLDFIRE\n") != NULL);
    CHECK(strstr(g_events, "park\n1:ssaction") == NULL);   /* the park comes first, once */
    CHECK(strstr(g_events, "1:setFireMode(3)\n1:setFireMode(4)\n1:setFireMode(0)\n1:pullTriggerRight()\n"
                           "1:releaseTriggerRight()\n") != NULL);
    /* the trigger: the engine's own state is read back after each order */
    CHECK(strstr(log, "pull true\npulled true\nreleased false\n") != NULL);
    CHECK(strstr(g_events, "1:ae_aIPullTrigger(noanim,EQUIP_RIGHT_HAND,1)\n"
                           "1:ae_aIPullTrigger(noanim,EQUIP_LEFT_HAND,1)\n1:releaseTriggerLeft()\n"
                           "1:pullTriggerLeft()\n") != NULL);
    CHECK(strstr(log, "gated false\ngated left false\nplain left true\n") != NULL);
    /* the muzzle test: the engine's answer and the inputs it read */
    CHECK(strstr(log, "muzzle true true 12.0 24.5 PRECISE true true true\n") != NULL);
    CHECK(g_ent[1].muzzle_mode == SH_AI_MUZZLE_FIRING && g_ent[1].muzzle_hand == SH_AI_HAND_LEFT);
    CHECK(strstr(log, "firing true\n") != NULL);
    /* the focus: an entity, a point, nothing */
    CHECK(strstr(g_events, "1:setFocus(e2,1,1,3)\n") != NULL);
    CHECK(g_ent[1].aimed && g_ent[1].aim[0] == 1 && g_ent[1].aim[1] == 2 && g_ent[1].aim[2] == 3);
    CHECK(strstr(g_events, "1:clearAimFocus()\n") != NULL);
    /* hold: the engine's ClearMove, then its idle with the turn type; the default is COMBAT */
    CHECK(strstr(g_events, "clear_move\n1:ssaction_Idle(-1,1)\nclear_move\n1:ssaction_Idle(-1,0)\n"
                           "clear_move\n1:ssaction_Idle(-1,2)\nclear_move\n1:ssaction_Idle(-1,2)\n") != NULL);
    CHECK(g_ent[1].clear_moves == 4);
    CHECK(strstr(log, "hold true\nhold2 true\n") != NULL && strstr(log, "hold3 false\n") != NULL);
    CHECK(strstr(log, "hold4 false | the demon's aibehavior decl has no turnParms entry for that turn\n") != NULL);
    CHECK(strstr(log, "\"hold\": the demon's aibehavior decl has no turnParms entry for AIIDLETURN_INCOVER, so the "
                      "game stands it still but never turns it in place; add one (turnParms item[2]) for it to "
                      "turn.") != NULL);
    CHECK(strstr(log, "no turnParms entry for AIIDLETURN_INCOVER") != NULL &&
          strstr(strstr(log, "no turnParms entry for AIIDLETURN_INCOVER") + 1, "no turnParms entry for AIIDLETURN_INCOVER") == NULL);   /* said once */
    /* the two switches */
    CHECK(strstr(log, "track true true\nset tracking true true\ntrack2 false true\n") != NULL);
    CHECK(strstr(g_events, "1:enableHeadTracking(0)\n1:enableBodyRotation(1)\n") != NULL);
    /* hold clears the script's record of its move */
    sh_script_map_end();

    /* body rotation off: the engine will not turn the demon; hold says so */
    CHECK(start(
        "api = 1\n"
        "on.think('ai/brute', function(self)\n"
        "  print('hold ' .. table.concat({tostring(self:hold())}, ' '))\n"
        "  local a, b = self:hold('COMBAT')\n"
        "  print('why ' .. tostring(a) .. ' ' .. tostring(b))\n"
        "end)\n"));
    g_ent[1].body_rot = 0;
    g_ent[1].focus_valid = 1;
    tick();
    log = log_of("aaaaaaaa");
    CHECK(strstr(log, "why false body rotation is off\n") != NULL);
    sh_script_map_end();

    /* a hold ends a move the script was waiting on */
    CHECK(start(
        "api = 1\n"
        "local step = 0\n"
        "on.think('ai/brute', function(self)\n"
        "  step = step + 1\n"
        "  if step == 1 then self:face({x = 500, y = 0, z = 0}) end\n"
        "  if step == 2 then self:hold() end\n"
        "  local d = self:move_done()\n"
        "  print('d ' .. step .. ' ' .. tostring(d) .. ' ' .. tostring(self:turn_done()))\n"
        "end)\n"));
    g_ent[1].move_status = 3;
    g_ent[1].yaw = 90;   /* facing away from the face target until the hold */
    tick(); tick();
    log = log_of("aaaaaaaa");
    CHECK(strstr(log, "d 1 false false\nd 2 true true\n") != NULL);
    sh_script_map_end();
}

/* A hand that is no hand, a muzzle test the session lacks, and the fire calls
 * with no engine function behind them: each answers with one plain sentence. */
static void test_fire_refusals(void)
{
    const char *log;
    printf("-- the fire control calls refuse plainly\n");
    expect_stop("self:fire_mode('BURST')", "\"fire_mode\" does not know the fire mode \"BURST\" (the game's AIFIREMODE_ "
                "names: HOLDFIRE, WEAPON_EXPLICIT_RELEASE, WEAPON, FIREWHENREADY, SPRAYWHENREADY)");
    expect_stop("self:fire_mode('AIFIREMODE_MAX')", "\"fire_mode\" does not know the fire mode \"AIFIREMODE_MAX\"");
    expect_stop("self:fire_mode(9)", "\"fire_mode\" needs a fire mode from 0 (HOLDFIRE) to 4 (SPRAYWHENREADY), got 9");
    expect_stop("self:fire_mode(true)", "\"fire_mode\" needs a fire mode name such as \"FIREWHENREADY\", got boolean");
    expect_stop("ent(3):fire_mode('HOLDFIRE')", "\"fire_mode\" works only on a demon, and this entity is not one");
    expect_stop("self:pull_trigger('yes')", "\"pull_trigger\" needs true or false for whether the game's fire check must "
                "pass first, got string");
    expect_stop("self:pull_trigger(true, 'third')", "\"pull_trigger\" needs the hand as \"left\" or \"right\"");
    expect_stop("self:release_trigger('third')", "\"release_trigger\" needs the hand as \"left\" or \"right\"");
    expect_stop("self:muzzle_on_target('hard')", "\"muzzle_on_target\" needs the test as \"pull\", \"firing\" or \"plain\", "
                "got hard");
    expect_stop("self:muzzle_on_target(5)", "\"muzzle_on_target\" needs the test as \"pull\", \"firing\" or \"plain\", got number");
    expect_stop("self:focus(5)", "\"focus\" needs an entity, a position or nil, got number");
    expect_stop("self:focus({x = 1, y = 'a', z = 3})", "\"focus\" needs a position with a number for y");
    expect_stop("self:hold('SPRINT')", "\"hold\" needs the turn as \"RELAXED\", \"COMBAT\" or \"INCOVER\" (the game's "
                "AIIDLETURN_ names), got SPRINT");
    expect_stop("self:hold(1)", "\"hold\" needs the turn as \"RELAXED\", \"COMBAT\" or \"INCOVER\" (the game's "
                "AIIDLETURN_ names), got number");
    expect_stop("self:tracking('on')", "\"tracking\" needs true or false, got string");
    expect_stop("self:body_rotation(1)", "\"body_rotation\" needs true or false, got number");

    /* a demon whose weapon hand is not a hand */
    CHECK(start("api = 1\non.think('ai/brute', function(self) self:pull_trigger() end)\n"));
    g_ent[1].weapon_hand = 7;
    tick();
    CHECK(strstr(stop_of("aaaaaaaa"), "\"pull_trigger\": this demon's weapon hand (weaponSlot in its aibehavior decl) "
                                      "is not a hand; name the hand, \"left\" or \"right\"") != NULL);
    sh_script_map_end();
    CHECK(start("api = 1\non.think('ai/brute', function(self) self:release_trigger() end)\n"));
    g_ent[1].weapon_hand = 7;
    tick();
    CHECK(strstr(stop_of("aaaaaaaa"), "\"release_trigger\": this demon's weapon hand") != NULL);
    sh_script_map_end();

    /* an engine without the game parts: the reads refuse, the hold still stands the demon, and the muzzle
     * test answers nil beside the inputs it could read */
    {
        sh_script_ai_engine bare = g_fake;
        bare.fire_modes = NULL; bare.focus_state = NULL; bare.muzzle_test = NULL; bare.clear_move = NULL;
        bare.idle_turn_entry = NULL;
        sh_script_ai_set_engine(&bare);
        expect_stop("self:fire_mode()", "\"fire_mode\" is not available in this session (Snapmap+ could not find the game "
                    "function it needs; see the backend log)");
        expect_stop("self:muzzle_on_target()", "\"muzzle_on_target\" is not available in this session");
        expect_stop("self:tracking()", "\"tracking\" is not available in this session");
        expect_stop("self:body_rotation()", "\"body_rotation\" is not available in this session");
        CHECK(start("api = 1\non.think('ai/brute', function(self) print('hold ' .. tostring(self:hold())) end)\n"));
        tick();
        CHECK(strstr(log_of("aaaaaaaa"), "hold true\n") != NULL);   /* nothing to say it will not turn */
        CHECK(strstr(g_events, "1:ssaction_Idle(-1,1)\n") != NULL && strstr(g_events, "clear_move") == NULL);
        sh_script_map_end();
        bare = g_fake;
        bare.muzzle_test = NULL;
        sh_script_ai_set_engine(&bare);
        CHECK(start("api = 1\non.think('ai/brute', function(self) local on, i = self:muzzle_on_target() "
                    "print('muzzle ' .. tostring(on) .. ' ' .. tostring(i.focus_valid)) end)\n"));
        g_ent[1].focus_valid = 1;
        tick();
        log = log_of("aaaaaaaa");
        CHECK(strstr(log, "muzzle nil true\n") != NULL);
        sh_script_map_end();
        sh_script_ai_set_engine(&g_fake);
    }
    /* the engine's muzzle answer unavailable for this weapon (no weapon in the hand): nil */
    CHECK(start("api = 1\non.think('ai/brute', function(self) print('muzzle ' .. tostring((self:muzzle_on_target()))) end)\n"));
    g_ent[1].muzzle = -1;
    tick();
    CHECK(strstr(log_of("aaaaaaaa"), "muzzle nil\n") != NULL);
    sh_script_map_end();
}

/* The SDK's weapon library and the probe's fire line, on the same calls. */
static void test_enemy_weapon_sdk(void)
{
    const char *log;
    printf("-- enemy_weapon and the probe's fire line\n");
    CHECK(start(
        "api = 1\n"
        "local W = require('enemy_weapon')\n"
        "local probe = require('enemy_probe')\n"
        "local step = 0\n"
        "on.think('ai/brute', function(self)\n"
        "  step = step + 1\n"
        "  if step == 1 then\n"
        "    print('equip ' .. tostring(W.equip(self, 'weapon/ai/brute/claw')))\n"
        "    W.fire_mode(self, 'FIREWHENREADY')\n"
        "    W.tick(self, 0.1)\n"
        "    W.tick(self, 0.1)\n"
        "    print('pull ' .. tostring(W.trigger(self, true, true)))\n"
        "    W.trigger(self, false)\n"
        "    print('ready ' .. tostring(W.ready(self)) .. ' pulled ' .. tostring(W.pulled(self)))\n"
        "    print('hold ' .. tostring(W.hold(self, 'COMBAT')))\n"
        "    local on, info = W.on_target(self, 'firing')\n"
        "    print('on ' .. tostring(on) .. ' ' .. tostring(info.focus_valid))\n"
        "    local st = W.state(self)\n"
        "    print('state ' .. st.mode .. ' ' .. st.anim_mode .. ' ' .. tostring(st.pulled) .. ' ' .. tostring(st.require_muzzle))\n"
        "    local w = probe.wrap({ projectile = function() end }, { print = print })\n"
        "    pcall(w.projectile, self, nil, 'rocket', {x = 0, y = 0, z = 0}, nil)\n"
        "    W.focus(self, {x = 5, y = 6, z = 7})\n"
        "  elseif step == 2 then\n"
        "    W.tick(self, 0.1)\n"
        "    W.focus(self, nil)\n"
        "    W.tick(self, 0.1)\n"
        "  end\n"
        "end)\n"));
    g_ent[1].sim = 1; g_ent[1].ready = 1; g_ent[1].gate = 1; g_ent[1].enemy = 2;
    g_ent[1].focus_valid = 1; g_ent[1].muzzle = 1; g_ent[1].require = 1; g_ent[1].focus_radius = 20.0f;
    g_ent[1].tracking = 1; g_ent[1].body_rot = 1;
    tick();
    g_ent[1].enemy = 3;
    tick();
    log = log_of("aaaaaaaa");
    if (g_fail) printf("sdk log:\n%s\nevents:\n%s\nstop: %s\n", log, g_events, stop_of("aaaaaaaa"));
    CHECK(sh_script_status("aaaaaaaa") == SH_SCRIPT_RUNNING);
    CHECK(strstr(log, "equip right\n") != NULL);
    CHECK(strstr(log, "pull true\nready true pulled false\nhold true\non true true\n") != NULL);
    CHECK(strstr(log, "state AIFIREMODE_FIREWHENREADY AIFIREMODE_HOLDFIRE false true\n") != NULL);
    CHECK(strstr(log, "probe fire AIFIREMODE_FIREWHENREADY AIFIREMODE_HOLDFIRE pulled=false ready=true focus=true "
                      "muzzle=true radius=20.0 precision=NORMAL require=true\n") != NULL);
    /* the library asks the engine and keeps no fire state: the focus follows the enemy once per enemy, a fixed
     * point is left alone, and nil gives the enemy back */
    CHECK(strstr(g_events, "1:equipInventoryItemByIndex(0)\n1:setFireMode(3)\n1:setFocus(e2,1,1,3)\n"
                           "1:ae_aIPullTrigger(noanim,EQUIP_RIGHT_HAND,1)\n1:releaseTriggerRight()\n"
                           "clear_move\n1:ssaction_Idle(-1,1)\n") != NULL);
    CHECK(strstr(g_events, "1:releaseTriggerRight()\nclear_move") != NULL);
    {
        const char *a = strstr(g_events, "1:setFocus(e2,1,1,3)\n");
        CHECK(a != NULL && strstr(a + 1, "1:setFocus(e2,1,1,3)\n") == NULL);
        CHECK(g_ent[1].aimed && g_ent[1].aim[0] == 5);   /* the fixed point went to the engine's aim-point call */
        CHECK(strstr(g_events, "1:clearAimFocus()\n1:setFocus(e3,1,1,3)\n") != NULL);   /* nil, then the enemy again */
    }
    sh_script_map_end();
}

/* A demon's own lasers and rear footsteps: the engine is asked by tag and by the mod's whole decl names. */
static void test_lasers_and_rear_feet(void)
{
    const char *log;
    printf("-- wound_hides names the wound by its whole decl name and the meshes as written\n");
    CHECK(start(
        "api = 1\n"
        "local step = 0\n"
        "on.think('ai/brute', function(self)\n"
        "  step = step + 1\n"
        "  if step ~= 1 then return end\n"
        "  print('hides ' .. tostring(self:wound_hides('wounds/brute/chest', { 'body$2@chest', 'body$3@chest' })))\n"
        "  print('none ' .. tostring(self:wound_hides('wounds/brute/armor', {})))\n"
        "end)\n"));
    tick();
    log = log_of("aaaaaaaa");
    CHECK(strstr(log, "hides true\nnone true\n") != NULL);
    CHECK(strstr(g_events, "wound_hides mods/aaaaaaaa/wounds/brute/chest: body$2@chest body$3@chest\n"
                           "wound_hides mods/aaaaaaaa/wounds/brute/armor:\n") != NULL);
    sh_script_map_end();
    g_no_wound = 1;
    CHECK(start(
        "api = 1\n"
        "local step = 0\n"
        "on.think('ai/brute', function(self)\n"
        "  step = step + 1\n"
        "  if step ~= 1 then return end\n"
        "  local ok, why = self:wound_hides('wounds/brute/chest', { 'a' })\n"
        "  print('hides ' .. tostring(ok) .. ' ' .. tostring(why))\n"
        "end)\n"));
    tick();
    CHECK(strstr(log_of("aaaaaaaa"), "hides false the wound is not a loaded gorewounds decl\n") != NULL);
    sh_script_map_end();
    g_no_wound = 0; g_no_mesh = 1;
    CHECK(start(
        "api = 1\n"
        "local step = 0\n"
        "on.think('ai/brute', function(self)\n"
        "  step = step + 1\n"
        "  if step ~= 1 then return end\n"
        "  local ok, why = self:wound_hides('wounds/brute/chest', { 'a' })\n"
        "  print('hides ' .. tostring(ok) .. ' ' .. tostring(why))\n"
        "end)\n"));
    tick();
    CHECK(strstr(log_of("aaaaaaaa"), "hides false the demon's model has no mesh of one of these names\n") != NULL);
    sh_script_map_end();
    g_no_mesh = 0;
    expect_stop("self:wound_hides('w', 'mesh')", "\"wound_hides\" needs a list of mesh names here, got string");
    expect_stop("self:wound_hides('w', { 1 })", "\"wound_hides\" needs mesh names in its list, got number at 1");

    printf("-- laser_add, laser_remove and rear_footsteps ask the engine by tag and whole decl name\n");
    CHECK(start(
        "api = 1\n"
        "local step = 0\n"
        "on.think('ai/brute', function(self)\n"
        "  step = step + 1\n"
        "  if step ~= 1 then return end\n"
        "  print('add ' .. tostring(self:laser_add('laser_source_1')))\n"
        "  print('add2 ' .. tostring(self:laser_add('muzzlereference', 'ai/tyrant/main_beam')))\n"
        "  print('add3 ' .. tostring(self:laser_add('muzzlereference', game('revenant/laser1'))))\n"
        "  print('remove ' .. tostring(self:laser_remove('laser_source_1')))\n"
        "  print('feet ' .. tostring(self:rear_footsteps('impacteffect/footsteps/ai_tyrant', 'impacteffect/footsteps/ai_tyrant_metal')))\n"
        "end)\n"));
    tick();
    log = log_of("aaaaaaaa");
    CHECK(strstr(log, "add true\nadd2 true\nadd3 true\nremove true\nfeet true\n") != NULL);
    CHECK(strstr(g_events, "laser_add laser_source_1 (decl)\n"
                           "laser_add muzzlereference mods/aaaaaaaa/ai/tyrant/main_beam\n"
                           "laser_add muzzlereference revenant/laser1\n"
                           "laser_remove laser_source_1\n"
                           "rear_footsteps mods/aaaaaaaa/impacteffect/footsteps/ai_tyrant -> "
                           "mods/aaaaaaaa/impacteffect/footsteps/ai_tyrant_metal\n") != NULL);
    sh_script_map_end();

    /* what the engine cannot do is answered with the reason, not an error */
    g_no_targeter = 1; g_no_ribbon = 1; g_no_table = 1;
    CHECK(start(
        "api = 1\n"
        "local step = 0\n"
        "on.think('ai/brute', function(self)\n"
        "  step = step + 1\n"
        "  if step ~= 1 then return end\n"
        "  print('add ' .. table.concat({tostring(self:laser_add('laser_source_1'))}, ' '))\n"
        "  local a, why = self:laser_add('laser_source_1')\n"
        "  print('why ' .. tostring(why))\n"
        "  local b, why2 = self:rear_footsteps('a', 'b')\n"
        "  print('feet ' .. tostring(b) .. ' ' .. tostring(why2))\n"
        "  print('remove ' .. tostring(self:laser_remove('laser_source_1')))\n"
        "end)\n"));
    tick();
    log = log_of("aaaaaaaa");
    CHECK(strstr(log, "add false\n") != NULL);
    CHECK(strstr(log, "why the demon has no laser targeter (its entityDef names no aicomponent_lasertargeter decl)\n") != NULL);
    CHECK(strstr(log, "feet false one of the two footstep tables is not a loaded projectileImpactEffect decl\n") != NULL);
    CHECK(strstr(log, "remove false\n") != NULL);
    sh_script_map_end();
    g_no_targeter = 0;
    CHECK(start("api = 1\non.think('ai/brute', function(self)\n"
                "  local a, why = self:laser_add('muzzlereference', 'ai/none')\n  print('ribbon ' .. tostring(a) .. ' ' .. why)\nend)\n"));
    tick();
    CHECK(strstr(log_of("aaaaaaaa"), "ribbon false no ribbon decl of that name is loaded\n") != NULL);
    sh_script_map_end();
    g_no_ribbon = 0; g_no_table = 0;

    expect_stop("self:laser_add()", "\"laser_add\" needs a model tag");
    expect_stop("self:laser_add('a b')", "is not a usable name");
    expect_stop("self:rear_footsteps('x')", "\"rear_footsteps\" needs a name here, got nothing");
    {
        sh_script_ai_engine bare = g_fake;
        bare.laser_add = NULL;
        bare.laser_remove = NULL;
        bare.rear_footsteps = NULL;
        sh_script_ai_set_engine(&bare);
        expect_stop("self:laser_add('t')", "\"laser_add\" is not available in this session");
        expect_stop("self:laser_remove('t')", "\"laser_remove\" is not available in this session");
        expect_stop("self:rear_footsteps('a', 'b')", "\"rear_footsteps\" is not available in this session");
        sh_script_ai_set_engine(&g_fake);
    }
}

static void test_brain_refusals(void)
{
    printf("-- the brain's calls refuse plainly\n");
    expect_stop("self:recoil(2)", "\"recoil\" needs a weight from 0 (no recoil) to 1 (full), got 2");
    expect_stop("self:aim_point(5)", "\"aim_point\" needs a position");
    expect_stop("self:anim_valid('evade/side', 'TF_FLY')", "\"anim_valid\" does not know the test \"TF_FLY\"");
    expect_stop("self:anim_valid('a b')", "\"anim_valid\": \"a b\" is not an animWeb node name");
    expect_stop("self:anim_valid('evade/side', 'TF_BOUNDS', -1)", "\"anim_valid\" needs the animation to try as a whole number");
    expect_stop("self:anim_force_via('idle', nil)", "\"anim_force_via\" needs the node to force first as \"subweb/state\" text, got nothing");
    expect_stop("self:firing_position({ kind = 'best' })", "\"firing_position\": kind is \"any\", \"new\", \"superior\" or \"reposition\"");
    expect_stop("self:firing_position({ near = 5 })", "\"firing_position\" knows target, kind, min, max, ideal, visible, count, path_weight and parms, not \"near\"");
    expect_stop("self:spline_clear(ent(2), 'shallow_right', 1)", "\"spline_clear\" needs the spline as a table");
    expect_stop("self:spline_clear(ent(2), { times = {0.5, 0.2}, values = {1} }, 1)", "\"spline_clear\" needs the spline's times rising from 0 to 1");
    expect_stop("self:spline_clear(ent(2), { times = {1}, values = {1} }, 1, 'TF_BOUNDS')", "\"spline_clear\" knows the flag \"TF_NAV_GROUND\" only");
    expect_stop("self:pain_string('a b')", "\"pain_string\": \"a b\" is not a usable string");
    expect_stop("self:block_pain('PAIN_SNEEZE', 1)", "\"block_pain\" does not know the pain reaction \"PAIN_SNEEZE\"");
    expect_stop("self:block_pain('falter', 9999)", "\"block_pain\" needs seconds from 0 to 600, got 9999");
}

/* anim_force_via: the via node is forced now; when the web has played it
 * and moved on, the destination is asked for; the observer reports the
 * via's blend in and out, then the destination. */
static void test_anim_force_via(void)
{
    const char *log;
    printf("-- anim_force_via\n");
    CHECK(start(
        "api = 1\n"
        "local step = 0\n"
        "on.think('ai/brute', function(self)\n"
        "  step = step + 1\n"
        "  if step == 1 then self:anim_force_via('rocket_combat/idle', 'evade/left', 0.25) end\n"
        "  print('s' .. step .. ' ' .. tostring(self:anim_event('VIA_BLEND_IN_START')) .. ' ' ..\n"
        "        tostring(self:anim_event('VIA_BLEND_OUT_START')) .. ' ' .. tostring(self:anim_event('DEST_REACHED')))\n"
        "end)\n"));
    strcpy(g_ent[1].node, "rocket_combat/walk");
    tick();                                                  /* forced */
    strcpy(g_ent[1].node, "evade/left"); tick();             /* the via plays */
    strcpy(g_ent[1].node, "evade/left_out"); tick();         /* the web left it: the destination is asked for */
    strcpy(g_ent[1].node, "rocket_combat/idle"); tick();     /* reached */
    log = log_of("aaaaaaaa");
    CHECK(strstr(g_events, "1:ssaction_ForceAnimState(0,evade/left,250,0)\n") != NULL);
    CHECK(strstr(g_events, "1:ssaction_ChangeAnimState(0,rocket_combat/idle,0)\n") != NULL);
    CHECK(strstr(g_events, "ChangeAnimState(0,rocket_combat/idle") > strstr(g_events, "ForceAnimState(0,evade/left"));
    CHECK(strstr(log, "s1 false false false\n") != NULL);
    CHECK(strstr(log, "s4 true true true\n") != NULL);
    sh_script_map_end();
}

/* An order given while a step of the engine's is in flight (a turn in place):
 * the engine ends the step's stage when its own animWeb command ends, and a
 * script's order goes round that command. */
/* A mod's demon is given its gore component when it spawns; another entity is left alone. */
static void test_a_spawned_demon_gets_its_gore(void)
{
    int was;
    g_gore_calls = 0;
    was = g_ent[1].is_ai;
    g_ent[1].is_ai = 1;
    sh_script_ai_spawned(&g_ent[1]);
    CHECK(g_gore_calls == 1);
    g_ent[1].is_ai = 0;
    sh_script_ai_spawned(&g_ent[1]);
    sh_script_ai_spawned(NULL);
    CHECK(g_gore_calls == 1);
    g_ent[1].is_ai = was;
}

static void test_orders_around_a_step(void)
{
    sh_ai_step_state s = { 1, SH_AI_STAGE_MOVE_STEP, 1, -1 };
    printf("-- orders around a step of the engine's\n");
    CHECK(sh_ai_step_in_flight(&s) == 1);
    s.stage = SH_AI_STAGE_ARRIVAL; CHECK(sh_ai_step_in_flight(&s) == 1);
    s.stage = SH_AI_STAGE_ARRIVAL_SOFT; CHECK(sh_ai_step_in_flight(&s) == 1);
    s.stage = 1; CHECK(sh_ai_step_in_flight(&s) == 0);            /* walking: no step */
    s.stage = SH_AI_STAGE_MOVE_STEP; s.waiting = 0; CHECK(sh_ai_step_in_flight(&s) == 0);   /* the command ended */
    s.waiting = 1; s.moving = 0; CHECK(sh_ai_step_in_flight(&s) == 0);                      /* not in the move state */
    CHECK(sh_ai_step_in_flight(NULL) == 0);

    /* an animation order waits for the turn; a hold with the standing turn type leaves the turn alone */
    CHECK(start(
        "api = 1\n"
        "local step = 0\n"
        "on.think('ai/brute', function(self)\n"
        "  step = step + 1\n"
        "  if step == 1 then self:hold() end\n"
        "  if step == 2 then print('hold ' .. tostring(self:hold())); self:anim_state('shoot/combo_cycle') end\n"
        "  if step == 3 then self:anim_state('shoot/combo_hold', 'shoot/combo_fire') end\n"
        "end)\n"));
    g_ent[1].body_rot = 1;
    tick();                                              /* held: clear_move, idle order */
    g_ent[1].moving = 1; g_ent[1].stage = SH_AI_STAGE_MOVE_STEP; g_ent[1].waiting = 1;
    g_ent[1].idle_turn_on = 1; g_ent[1].idle_turn = SH_AI_IDLETURN_COMBAT;   /* the engine turns it in place */
    g_events[0] = 0;
    tick();                                              /* hold and an order during the turn */
    CHECK(strstr(log_of("aaaaaaaa"), "hold true\n") != NULL);
    CHECK(g_events[0] == 0);                             /* nothing reached the engine */
    tick();                                              /* a newer order takes the held one's place */
    CHECK(g_events[0] == 0);
    g_ent[1].waiting = 0; g_ent[1].stage = 1;            /* the turn ended; the engine may ask for another */
    tick();
    CHECK(!strcmp(g_events, "clear_move\n1:ssaction_ChangeAnimStateVia(0,shoot/combo_hold,shoot/combo_fire,0)\n"));
    g_events[0] = 0;
    tick();                                              /* given once */
    CHECK(g_events[0] == 0);
    sh_script_map_end();

    /* a forced order is given at once and the step's command is ended the engine's way; a move drops a held order */
    memset(&g_ent[1].moving, 0, sizeof(int) * 5);
    CHECK(start(
        "api = 1\n"
        "local step = 0\n"
        "on.think('ai/brute', function(self)\n"
        "  step = step + 1\n"
        "  if step == 1 then self:anim_state('rocket_combat/idle'); self:anim_force('pain/left', 0.1) end\n"
        "  if step == 2 then self:anim_state('rocket_combat/idle'); self:move_to({x = 100, y = 0, z = 0}) end\n"
        "end)\n"));
    g_ent[1].moving = 1; g_ent[1].stage = SH_AI_STAGE_MOVE_STEP; g_ent[1].waiting = 1;
    g_ent[1].interrupts = 0;
    g_events[0] = 0;
    tick();
    CHECK(strstr(g_events, "ssaction_ChangeAnimState(") == NULL);
    CHECK(strstr(g_events, "1:ssaction_ForceAnimState(0,pain/left,100,0)\nstep_interrupt\n") != NULL);
    CHECK(g_ent[1].interrupts == 1);
    g_ent[1].waiting = 1;                                /* another step in flight */
    g_events[0] = 0;
    tick();                                              /* the order is held, then the move takes the web */
    CHECK(strstr(g_events, "ssaction_MoveToPoint(") != NULL && strstr(g_events, "ssaction_ChangeAnimState(") == NULL);
    g_ent[1].waiting = 0;
    g_events[0] = 0;
    tick();
    CHECK(strstr(g_events, "ssaction_ChangeAnimState(") == NULL);   /* dropped with the move */
    sh_script_map_end();
    memset(&g_ent[1].moving, 0, sizeof(int) * 5);
}

static void test_pain_records(void)
{
    int a, b;
    printf("-- per-demon pain records\n");
    sh_ai_pain_records_reset();
    CHECK(sh_ai_pain_string_get(&a, 5) == NULL);
    CHECK(sh_ai_pain_string_set(&a, 5, "combo") && !strcmp(sh_ai_pain_string_get(&a, 5), "combo"));
    CHECK(sh_ai_pain_string_get(&a, 6) == NULL);              /* a new entity in the same memory */
    CHECK(sh_ai_pain_string_get(&b, 5) == NULL);
    CHECK(sh_ai_pain_string_set(&a, 5, NULL) && sh_ai_pain_string_get(&a, 5) == NULL);
    CHECK(sh_ai_block_set(&a, 5, 7, 10000));
    CHECK(sh_ai_blocked(&a, 5, 7, 9999) && !sh_ai_blocked(&a, 5, 7, 10000) && !sh_ai_blocked(&a, 5, 6, 0));
    CHECK(!sh_ai_blocked(&b, 5, 7, 0) && !sh_ai_blocked(&a, 6, 7, 0));
    CHECK(!sh_ai_block_set(&a, 5, 0, 1) && !sh_ai_block_set(&a, 5, SH_AI_PAIN_REACTIONS, 1));
    sh_ai_pain_records_reset();
    CHECK(!sh_ai_blocked(&a, 5, 7, 0));
}

/* The pain-string stub on a synthetic pain request: the two loads the stub
 * replaces, then the request reads its string from [rbp+0x88]. */
static const char *g_stub_answer;
static void *g_stub_seen;
static const char *stub_fetch(void *ai) { g_stub_seen = ai; return g_stub_answer; }

static void test_pain_stub(void)
{
    static const uint8_t fn_head[] = { 0x53, 0x55, 0x48, 0x89, 0xCB, 0x48, 0x89, 0xD5 };   /* push rbx, rbp; rbx=rcx; rbp=rdx */
    static const uint8_t fn_tail[] = { 0x48, 0x89, 0x4D, 0x50,                           /* mov [rbp+0x50],rcx */
                                       0x48, 0x8B, 0x85, 0x88, 0x00, 0x00, 0x00,         /* mov rax,[rbp+0x88] */
                                       0x5D, 0x5B, 0xC3 };
    uint8_t *mem = (uint8_t *)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    uint8_t *fn, *site, *tramp, *stub;
    static uint8_t ai[0xC200], decl[0x100], frame[0x100];
    const char *(*call)(void *, void *);
    const char *engine_string = "engine", *r;
    void *p;
    size_t n;
    printf("-- the pain-string stub\n");
    CHECK(mem != NULL);
    if (!mem) return;
    fn = mem; site = fn + sizeof fn_head; tramp = mem + 256; stub = mem + 512;
    memcpy(fn, fn_head, sizeof fn_head);
    memcpy(site, k_ai_pain_stolen, SH_AI_PAIN_STUB_STOLEN);
    memcpy(site + SH_AI_PAIN_STUB_STOLEN, fn_tail, sizeof fn_tail);
    /* the trampoline: the stolen loads, then back after them */
    memcpy(tramp, k_ai_pain_stolen, SH_AI_PAIN_STUB_STOLEN);
    tramp[14] = 0xFF; tramp[15] = 0x25; memset(tramp + 16, 0, 4);
    p = site + SH_AI_PAIN_STUB_STOLEN; memcpy(tramp + 20, &p, 8);
    n = sh_ai_pain_stub_build(stub, SH_AI_PAIN_STUB_SIZE, (const void *)stub_fetch, tramp);
    CHECK(n > 0 && n <= SH_AI_PAIN_STUB_SIZE);
    CHECK(sh_ai_pain_stub_build(stub, 100, (const void *)stub_fetch, tramp) == 0);
    /* the 14-byte jump at the site */
    site[0] = 0xFF; site[1] = 0x25; memset(site + 2, 0, 4);
    p = stub; memcpy(site + 6, &p, 8);
    FlushInstructionCache(GetCurrentProcess(), mem, 4096);
    p = decl; memcpy(ai + 0xC120, &p, 8);
    p = (void *)(uintptr_t)0x1234; memcpy(decl + 0x90, &p, 8);
    memcpy(frame + 0x88, &engine_string, 8);
    call = (const char *(*)(void *, void *))fn;
    g_stub_answer = "combo";
    r = call(ai, frame);
    CHECK(r == g_stub_answer && g_stub_seen == ai);
    CHECK(*(uint64_t *)(frame + 0x50) == 0x1234);          /* the stolen loads still ran */
    memcpy(frame + 0x88, &engine_string, 8);
    g_stub_answer = NULL;
    r = call(ai, frame);
    CHECK(r == engine_string);                              /* no script string: the engine's own */
    VirtualFree(mem, 0, MEM_RELEASE);
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
    CHECK(sh_script_ai_register() == 46);
    sh_script_ai_set_engine(&g_fake);
    sh_script_api_set_engine(&g_common);
    sh_script_api_register();
    sh_script_handles_set_resolver(fake_resolve);

    test_catalogue(argc > 1 ? argv[1] : "fixtures\\scripts\\event_registry_13954591.txt");
    test_args();
    test_move_state();
    test_geometry();
    test_commands();
    test_move_done_and_follow();
    test_queries();
    test_refusals();
    test_long_tag_names();
    test_turn_done();
    test_removed_and_unavailable();
    test_brain_pure();
    test_compositions();
    test_long_path();
    test_weapon_hands();
    test_face_travel();
    test_sight_against_a_wall();
    test_brain_calls();
    test_brain_refusals();
    test_fire_calls();
    test_fire_refusals();
    test_lasers_and_rear_feet();
    test_enemy_weapon_sdk();
    test_anim_force_via();
    test_orders_around_a_step();
    test_a_spawned_demon_gets_its_gore();
    test_pain_records();
    test_pain_stub();
    files_clear();
    CHECK(sh_script_bytes_in_use() == 0);
    printf(g_fail ? "script_api_ai_test: FAILED\n" : "script_api_ai_test: ok\n");
    return g_fail;
}
