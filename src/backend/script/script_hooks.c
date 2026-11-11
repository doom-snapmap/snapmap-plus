/* script_hooks.c -- see script_hooks.h. The pure part (the mod table, the
 * owner match, the tick's and gates' decisions) builds alone with
 * SH_SCRIPT_PURE for tests/script_hooks_test.c; the engine part needs the
 * backend. */
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "script_hooks.h"

/* ---- the mods of this map ---------------------------------------------- */

typedef struct mod_slot {
    int used;
    char salt[SH_SCRIPT_SALT_MAX + 1];
    char name[SH_SCRIPT_NAME_MAX];
} mod_slot;

static mod_slot g_mods[SH_SCRIPT_MODS_MAX];
static int g_mod_count;

static void hooks_log(const char *fmt, ...);

static int salt_ok(const char *s)
{
    size_t n = s ? strlen(s) : 0;
    size_t i;
    if (!n || n > SH_SCRIPT_SALT_MAX) return 0;
    for (i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return 0;
    }
    return 1;
}

int sh_script_hooks_mod_add(const char *salt, const char *display_name)
{
    int i, free_at = -1;
    if (!salt_ok(salt)) {
        hooks_log("Mod scripts: a mod with scripts has no valid identity, so its scripts do not run in this map.");
        return -1;
    }
    for (i = 0; i < SH_SCRIPT_MODS_MAX; i++) {
        if (g_mods[i].used && !_stricmp(g_mods[i].salt, salt)) return i;
        if (!g_mods[i].used && free_at < 0) free_at = i;
    }
    if (free_at < 0) {
        hooks_log("Mod scripts: %s has scripts but this map already runs scripts from %d mods, the most one map "
                  "can; its scripts do not run here.", display_name && display_name[0] ? display_name : salt,
                  SH_SCRIPT_MODS_MAX);
        return -1;
    }
    if (!sh_script_queue_open(free_at)) {
        hooks_log("Mod scripts: %s's scripts do not run in this map because there was not enough memory for them.",
                  display_name && display_name[0] ? display_name : salt);
        return -1;
    }
    memset(&g_mods[free_at], 0, sizeof g_mods[free_at]);
    g_mods[free_at].used = 1;
    strncpy_s(g_mods[free_at].salt, sizeof g_mods[free_at].salt, salt, _TRUNCATE);
    strncpy_s(g_mods[free_at].name, sizeof g_mods[free_at].name,
              display_name && display_name[0] ? display_name : salt, _TRUNCATE);
    g_mod_count++;
    return free_at;
}

int sh_script_hooks_mod_count(void)
{
    return g_mod_count;
}

const char *sh_script_hooks_mod_salt(int mod)
{
    return mod >= 0 && mod < SH_SCRIPT_MODS_MAX && g_mods[mod].used ? g_mods[mod].salt : NULL;
}

const char *sh_script_hooks_mod_name(int mod)
{
    return mod >= 0 && mod < SH_SCRIPT_MODS_MAX && g_mods[mod].used ? g_mods[mod].name : NULL;
}

static void mods_clear(void)
{
    memset(g_mods, 0, sizeof g_mods);
    g_mod_count = 0;
    sh_script_queue_close_all();
}

static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

/* The salt of "mods/<salt>/..." (either separator) into out; 0 when the name
 * is not in that shape. */
static int salt_of(const char *name, char *out, size_t cap)
{
    size_t n = 0;
    if (!name || lower(name[0]) != 'm' || lower(name[1]) != 'o' || lower(name[2]) != 'd' || lower(name[3]) != 's' ||
        (name[4] != '/' && name[4] != '\\'))
        return 0;
    name += 5;
    while (name[n] && name[n] != '/' && name[n] != '\\') {
        if (n + 1 >= cap) return 0;
        out[n] = name[n];
        n++;
    }
    if (!n || !name[n] || !name[n + 1]) return 0;   /* a salt and something under it */
    out[n] = 0;
    return 1;
}

int sh_script_hooks_owner_of(const char *const *names, int count)
{
    char salt[SH_SCRIPT_SALT_MAX + 2];
    int i, m;
    if (!names || count <= 0 || !g_mod_count) return -1;
    for (i = 0; i < count; i++) {
        if (!salt_of(names[i], salt, sizeof salt)) continue;
        for (m = 0; m < SH_SCRIPT_MODS_MAX; m++)
            if (g_mods[m].used && !_stricmp(g_mods[m].salt, salt)) return m;
    }
    return -1;
}

/* ---- decisions ------------------------------------------------------------ */

int sh_script_tick_decide(const sh_script_tick_state *s)
{
    if (!s || s->open <= 0) return SH_SCRIPT_TICK_IDLE;
    if (s->in_tick) return SH_SCRIPT_TICK_REENTERED;
    if (s->on_main != 1) return s->on_main == 0 ? SH_SCRIPT_TICK_OFF_MAIN : SH_SCRIPT_TICK_UNKNOWN;
    if (s->is_client == 1) return SH_SCRIPT_TICK_CLIENT;
    if (s->is_client != 0) return SH_SCRIPT_TICK_UNKNOWN;
    if (s->loading == 1) return SH_SCRIPT_TICK_LOADING;
    if (s->loading != 0) return SH_SCRIPT_TICK_UNKNOWN;
    if (s->in_play == 1) return SH_SCRIPT_TICK_RUN;
    if (s->in_play == 0) return s->started ? SH_SCRIPT_TICK_END : SH_SCRIPT_TICK_NOT_PLAY;
    return SH_SCRIPT_TICK_UNKNOWN;
}

const char *sh_script_tick_name(int d)
{
    switch (d) {
    case SH_SCRIPT_TICK_RUN: return "run";
    case SH_SCRIPT_TICK_END: return "end";
    case SH_SCRIPT_TICK_CLIENT: return "client";
    case SH_SCRIPT_TICK_IDLE: return "idle";
    case SH_SCRIPT_TICK_REENTERED: return "re-entered";
    case SH_SCRIPT_TICK_OFF_MAIN: return "off the main thread";
    case SH_SCRIPT_TICK_UNKNOWN: return "state unknown";
    case SH_SCRIPT_TICK_LOADING: return "loading";
    case SH_SCRIPT_TICK_NOT_PLAY: return "not in Play";
    default: return "?";
    }
}

int sh_script_gate_opens(const char *phase)
{
    return phase && (!strcmp(phase, "map") || !strcmp(phase, "play"));
}

#ifdef SH_SCRIPT_PURE

static void hooks_log(const char *fmt, ...)
{
    (void)fmt;
}

/* The tests drive the mod table directly. */
void sh_script_hooks_test_reset(void)
{
    mods_clear();
}

#else /* the engine side ------------------------------------------------- */

#include "../backend_log.h"
#include "../entity.h"
#include "../hook.h"
#include "../modserve.h"
#include "../engine_globals.h"
#include "../companion_engine.h"
#include "../../fault_shield/load_screen_stop.h"

#define LOG_LINES_MAX            64
#define LOAD_SCREEN_RUNNING_BYTE 0x4B2   /* idLoadScreen thread: 1 while it draws (load_screen_stop.c) */
#define SWEEP_BATCH              256

typedef void (*tick_fn)(void *superscript);
typedef unsigned char (*is_client_fn)(void *gm);
typedef void *(*lookup_fn)(void *gm, const uint32_t *spawn_id);

static tick_fn       g_orig_tick;
static void         *g_tick_target;
static is_client_fn  g_is_client;
static void         *g_lookup;           /* idGameLocal vtable +0x7F0, checked against the live vtable */
static const uint32_t *g_main_tid;
static const uint8_t *g_load_screen;     /* idLoadScreen; its first field is the thread object */
static int           g_resolved;

static int           g_open;             /* states this map opened */
static int           g_started;          /* the runtime had a tick this map */
static int           g_in_tick;
static int           g_close_pending;    /* CancelMapChange during the tick: close after it */
static int           g_close_pending_handlers;
static uint32_t      g_frame;
static volatile LONG g_routing;          /* records are accepted for this map's states */
static volatile LONG g_full_logged;
static int           g_log_lines;
static int           g_vtable_checked;   /* 1 verified, -1 refused */

static void hooks_log(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    if (g_log_lines >= LOG_LINES_MAX) return;
    g_log_lines++;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof line, _TRUNCATE, fmt, ap);
    va_end(ap);
    backend_log(line);
}

/* Only access violations are contained; an engine idException must reach
 * the engine's own handler. */
static int av_only(DWORD code)
{
    return code == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}

static void *read_ptr(const void *base, size_t off)
{
    void *p = NULL;
    __try { p = *(void * const *)((const uint8_t *)base + off); }
    __except (av_only(GetExceptionCode())) { p = NULL; }
    return p;
}

static void *vfn(void *obj, size_t slot)
{
    void **vt = obj ? (void **)read_ptr(obj, 0) : NULL;
    return vt ? read_ptr(vt, slot) : NULL;
}

static int on_main(void)
{
    uint32_t id = 0;
    if (!g_main_tid) return -1;
    __try { id = *(const volatile uint32_t *)g_main_tid; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    if (!id) return -1;
    return id == GetCurrentThreadId() ? 1 : 0;
}

static int loading(void)
{
    if (!g_load_screen) return -1;
    __try {
        const uint8_t *thread = *(const uint8_t *const volatile *)g_load_screen;
        if (!thread) return 0;
        return *(const volatile uint8_t *)(thread + LOAD_SCREEN_RUNNING_BYTE) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

static int is_client(void)
{
    void *gm = sh_entity_gamemgr();
    if (!gm || !g_is_client) return -1;
    return g_is_client(gm) ? 1 : 0;
}

/* idGameLocal vtable +0x7F8: the +0x7F0 lookup, then the idEntity cast
 * (VK 0x1403912A0, GL 0x140390B70). Used once the live game's +0x7F0 slot is
 * proven to hold the resolved lookup (checked once, at the first resolve). */
static int vtable_ok(void);

static void *resolve_spawn_id(uint32_t spawn_id)
{
    void *gm = sh_entity_gamemgr();
    lookup_fn f;
    if (!gm || !vtable_ok()) return NULL;
    f = (lookup_fn)vfn(gm, SH_SCRIPT_GAME_LOOKUP_SLOT + 8);
    return f ? f(gm, &spawn_id) : NULL;
}

static int vtable_ok(void)
{
    void *gm;
    if (g_vtable_checked) return g_vtable_checked == 1;
    gm = sh_entity_gamemgr();
    if (!gm) return 0;
    g_vtable_checked = vfn(gm, SH_SCRIPT_GAME_LOOKUP_SLOT) == g_lookup ? 1 : -1;
    if (g_vtable_checked != 1)
        hooks_log("Mod scripts are unavailable: the game's entity lookup is not where this version of Snapmap+ "
                  "expects it.");
    return g_vtable_checked == 1;
}

static uint32_t spawn_id_of(void *entity)
{
    void *gm = sh_entity_gamemgr();
    uint32_t id = SH_SCRIPT_SPAWN_ID_NONE;
    if (!gm || !entity) return id;
    __try {
        int num = *(const int *)((const uint8_t *)entity + SH_SCRIPT_ENT_NUMBER);
        if (num >= 0 && num < SH_SCRIPT_ENT_NUMBER_WORLD)
            id = *(const uint32_t *)((const uint8_t *)gm + SH_SCRIPT_GAME_SPAWN_IDS + (size_t)num * 4);
    } __except (av_only(GetExceptionCode())) {
        id = SH_SCRIPT_SPAWN_ID_NONE;
    }
    return id;
}

/* End this map's scripts: the runtime runs end handlers when asked, then
 * every state closes; records stop routing first. */
static void map_close(int run_end_handlers)
{
    if (!g_open) return;
    InterlockedExchange(&g_routing, 0);
    sh_script_map_close(run_end_handlers);
    mods_clear();
    sh_script_handles_reset();
    g_open = 0;
    g_started = 0;
    g_frame = 0;
    g_close_pending = 0;
}

static int prologue_ok(void)
{
    int same = 0;
    __try { same = !memcmp(g_tick_target, k_script_tick_prologue, SH_SCRIPT_TICK_STOLEN); }
    __except (EXCEPTION_EXECUTE_HANDLER) { same = 0; }
    return same;
}

static void report_drops(void)
{
    int m, first;
    for (m = 0; m < SH_SCRIPT_MODS_MAX; m++) {
        long n;
        if (!g_mods[m].used) continue;
        n = sh_script_queue_take_dropped(m, &first);
        if (n && first)
            hooks_log("Mod scripts: %s missed %ld game events because more than %d arrived between two frames; "
                      "its scripts may react late or not at all to some of them.", g_mods[m].name, n,
                      SH_SCRIPT_QUEUE_CAP);
    }
}

static void run_tick(void)
{
    sh_script_tick_state s;
    int d;
    memset(&s, 0, sizeof s);
    s.open = g_open;
    if (!s.open) return;
    s.in_tick = g_in_tick;
    s.on_main = on_main();
    s.is_client = s.on_main == 1 ? is_client() : -1;
    s.loading = loading();
    s.in_play = sh_modserve_engine_in_play();
    s.started = g_started;
    d = sh_script_tick_decide(&s);
    switch (d) {
    case SH_SCRIPT_TICK_RUN:
        g_in_tick = 1;
        g_started = 1;
        g_frame++;
        sh_script_queue_set_drain_thread(GetCurrentThreadId());
        report_drops();
        __try {
            sh_script_tick();
        } __except (av_only(GetExceptionCode())) {
            hooks_log("Mod scripts stopped for this map after an internal fault in Snapmap+; the game goes on. "
                      "Please report it with this log.");
            g_close_pending = 1;
            g_close_pending_handlers = 0;
        }
        g_in_tick = 0;
        if (g_close_pending) map_close(g_close_pending_handlers);
        break;
    case SH_SCRIPT_TICK_END:
        map_close(1);
        break;
    case SH_SCRIPT_TICK_CLIENT:
        hooks_log("Mod scripts: this machine joined as a client, so the host runs the map's scripts and this "
                  "machine runs none.");
        map_close(0);
        break;
    default:
        break;
    }
}

/* The SuperScript per-frame wrapper (script_hooks.h: main thread, inside
 * idGameLocal::RunFrame after the entity think loop). */
static void tick_detour(void *superscript)
{
    g_orig_tick(superscript);
    run_tick();
}

static int commit_tick(const char *phase)
{
    void *tramp;
    if (g_orig_tick && hook_is_installed((void *)g_orig_tick)) return 1;
    if (!prologue_ok()) {
        hooks_log("Mod scripts are unavailable: the game's frame function is not the expected one (another tool "
                  "may have changed it).");
        return 0;
    }
    tramp = hook_prepare(g_tick_target, (void *)tick_detour, SH_SCRIPT_TICK_STOLEN);
    if (!tramp) {
        hooks_log("Mod scripts are unavailable: the frame hook could not be prepared (%s).", hook_last_failure());
        return 0;
    }
    g_orig_tick = (tick_fn)tramp;
    if (hook_commit(tramp) != B2_PATCH_OK) {
        if (hook_unpatch(tramp)) g_orig_tick = NULL;
        hooks_log("Mod scripts are unavailable: the frame hook could not be installed.");
        return 0;
    }
    hooks_log("SCRIPTS: frame hook installed at %p (%s)", g_tick_target, phase);
    return 1;
}

void sh_script_hooks_gate(const char *phase)
{
    int n;
    if (!g_resolved || !sh_script_gate_opens(phase)) return;
    if (on_main() != 1) return;   /* every gate that opens is a main-thread gate; another caller is ignored */
    if (g_in_tick) return;        /* a gate cannot run inside the frame; never close under the runtime */
    map_close(0);                 /* the departed map's entities are gone at a map gate */
    InterlockedExchange(&g_full_logged, 0);
    g_log_lines = 0;
    sh_script_handles_reset();
    n = sh_script_map_open();
    if (n <= 0) {
        mods_clear();
        return;
    }
    if (!commit_tick(phase)) {
        sh_script_map_close(0);
        mods_clear();
        return;
    }
    g_open = n;
    InterlockedExchange(&g_routing, 1);
}

int sh_script_hooks_spawn_wanted(void)
{
    return g_open > 0;
}

void sh_script_hooks_after_spawn(void *entity, const char *const *names, int count)
{
    int mod, full = 0;
    uint32_t id;
    if (!g_open || !entity) return;
    mod = sh_script_hooks_owner_of(names, count);
    if (mod < 0) return;
    if (is_client() == 1) return;   /* a client's entities are the server's; it runs no script */
    id = spawn_id_of(entity);
    if (!sh_script_handles_add(mod, id, &full) && full && !InterlockedExchange(&g_full_logged, 1))
        hooks_log("Mod scripts: this map spawned more than %d entities of mods with scripts; %s's later ones are "
                  "not driven by its scripts.", SH_SCRIPT_HANDLES_MAX, g_mods[mod].name);
}

void sh_script_hooks_map_cancel(int during_load)
{
    if (!g_open) return;
    if (g_in_tick) {
        g_close_pending = 1;
        g_close_pending_handlers = 0;
        return;
    }
    map_close(!during_load && g_started);
}

sh_script_handle sh_script_hooks_handle_of(void *entity, int *mod)
{
    if (mod) *mod = -1;
    if (!g_routing || !entity) return 0;
    return sh_script_handles_find(spawn_id_of(entity), mod);
}

uint32_t sh_script_hooks_frame(void)
{
    return g_frame;
}

static void *own(const sig_result *r, int i)
{
    return r[i].status == SIG_OK ? (void *)r[i].addr : NULL;
}

int sh_script_hooks_install(const sig_result *results, size_t count, const uint8_t *module_base)
{
    sig_result r[SH_SCRIPT_SIG_COUNT];
    int i;
    (void)results; (void)count;
    if (g_resolved) return 1;
    if (!module_base) return 0;
    for (i = 0; i < SH_SCRIPT_SIG_COUNT; i++) {
        if (sig_resolve_one(module_base, &SH_SCRIPT_SIGNATURES[i], &r[i]) != SIG_OK && i <= SH_SCRIPT_SIG_IS_CLIENT) {
            hooks_log("SCRIPTS: %s did not resolve on this executable; mod scripts are unavailable",
                      SH_SCRIPT_SIGNATURES[i].name);
            return 0;
        }
    }
    g_tick_target = own(r, SH_SCRIPT_SIG_TICK);
    g_lookup = own(r, SH_SCRIPT_SIG_SPAWN_ID_LOOKUP);
    g_is_client = (is_client_fn)own(r, SH_SCRIPT_SIG_IS_CLIENT);
    g_main_tid = (const uint32_t *)glb_resolve(module_base, "main_thread_id", NULL);
    g_load_screen = (const uint8_t *)glb_resolve(module_base, "load_screen", NULL);
    if (!g_main_tid || !g_load_screen) {
        hooks_log("SCRIPTS: the main_thread_id or load_screen global did not resolve; mod scripts are unavailable");
        return 0;
    }
    if (!prologue_ok()) {
        hooks_log("SCRIPTS: the frame function's prologue is not the expected one (another detour?); mod scripts "
                  "are unavailable");
        return 0;
    }
    sh_script_handles_set_resolver(resolve_spawn_id);
    sh_companions_set_spawn_subscriber(sh_script_hooks_after_spawn, sh_script_hooks_spawn_wanted);
    sh_companions_set_gate_subscriber(sh_script_hooks_gate);
    sh_load_screen_stop_set_map_end(sh_script_hooks_map_cancel);
    g_resolved = 1;
    backend_log("SCRIPTS: engine side resolved; the frame hook is installed at the first map with scripts");
    return 1;
}

#endif /* SH_SCRIPT_PURE */
