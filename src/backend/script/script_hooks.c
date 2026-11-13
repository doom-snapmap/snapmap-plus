/* script_hooks.c -- see script_hooks.h. The pure part (the mod table, the
 * owner match, the tick's and gates' decisions) builds alone with
 * SH_SCRIPT_PURE for tests/script_hooks_test.c; the engine part needs the
 * backend. */
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../common/text.h"
#include "script_hooks.h"

/* ---- the mods of this map ---------------------------------------------- */

typedef struct mod_slot {
    int used;
    char salt[SH_SCRIPT_SALT_MAX + 1];
    char *name;              /* malloc'd display name */
} mod_slot;

/* The table grows with the map's scripted mods. Each slot is allocated once
 * and kept for the session, so a slot pointer read under the lock stays
 * valid; hooks on job threads read it (a record's mod salt), so the slot
 * array itself is read under g_mods_lock. */
static mod_slot **g_mods;
static int g_nslots, g_cslots;
static int g_mod_count;
static SRWLOCK g_mods_lock = SRWLOCK_INIT;

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

static mod_slot *slot_at(int mod)
{
    mod_slot *m = NULL;
    AcquireSRWLockShared(&g_mods_lock);
    if (mod >= 0 && mod < g_nslots && g_mods[mod]->used) m = g_mods[mod];
    ReleaseSRWLockShared(&g_mods_lock);
    return m;
}

/* A free slot, made when every slot is used; -1 without memory (or past the
 * handle table's mod number range, 0x7FFF). Main thread. */
static int free_slot(void)
{
    int i;
    mod_slot *m;
    for (i = 0; i < g_nslots; i++) if (!g_mods[i]->used) return i;
    if (g_nslots >= 0x7FFF) return -1;
    m = (mod_slot *)calloc(1, sizeof *m);
    if (!m) return -1;
    AcquireSRWLockExclusive(&g_mods_lock);
    if (g_nslots == g_cslots) {
        int cap = g_cslots ? g_cslots * 2 : 16;
        mod_slot **grown = (mod_slot **)realloc(g_mods, (size_t)cap * sizeof *grown);
        if (!grown) { ReleaseSRWLockExclusive(&g_mods_lock); free(m); return -1; }
        g_mods = grown;
        g_cslots = cap;
    }
    g_mods[g_nslots] = m;
    i = g_nslots++;
    ReleaseSRWLockExclusive(&g_mods_lock);
    return i;
}

int sh_script_hooks_mod_add(const char *salt, const char *display_name)
{
    int i, at;
    const char *shown = display_name && display_name[0] ? display_name : salt;
    char *name;
    if (!salt_ok(salt)) {
        hooks_log("Mod scripts: a mod with scripts has no valid identity, so its scripts do not run in this map.");
        return -1;
    }
    i = sh_script_hooks_mod_find(salt);
    if (i >= 0) return i;
    at = free_slot();
    name = at >= 0 ? _strdup(shown) : NULL;
    if (at < 0 || !name || !sh_script_queue_open(at)) {
        free(name);
        hooks_log("Mod scripts: %s's scripts do not run in this map because there was not enough memory for them.",
                  shown);
        return -1;
    }
    AcquireSRWLockExclusive(&g_mods_lock);
    free(g_mods[at]->name);
    memset(g_mods[at], 0, sizeof *g_mods[at]);
    g_mods[at]->used = 1;
    strncpy_s(g_mods[at]->salt, sizeof g_mods[at]->salt, salt, _TRUNCATE);
    g_mods[at]->name = name;
    g_mod_count++;
    ReleaseSRWLockExclusive(&g_mods_lock);
    return at;
}

int sh_script_hooks_mod_count(void)
{
    return g_mod_count;
}

const char *sh_script_hooks_mod_salt(int mod)
{
    mod_slot *m = slot_at(mod);
    return m ? m->salt : NULL;
}

const char *sh_script_hooks_mod_name(int mod)
{
    mod_slot *m = slot_at(mod);
    return m ? m->name : NULL;
}

int sh_script_hooks_mod_find(const char *salt)
{
    int i, found = -1;
    if (!salt) return -1;
    AcquireSRWLockShared(&g_mods_lock);
    for (i = 0; i < g_nslots && found < 0; i++)
        if (g_mods[i]->used && !_stricmp(g_mods[i]->salt, salt)) found = i;
    ReleaseSRWLockShared(&g_mods_lock);
    return found;
}

static void mods_clear(void)
{
    int i;
    AcquireSRWLockExclusive(&g_mods_lock);
    for (i = 0; i < g_nslots; i++) g_mods[i]->used = 0;   /* the name stays until the slot is used again */
    g_mod_count = 0;
    ReleaseSRWLockExclusive(&g_mods_lock);
    sh_script_queue_close_all();
}

void sh_script_hooks_mods_reset(void)
{
    mods_clear();
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
        if ((m = sh_script_hooks_mod_find(salt)) >= 0) return m;
    }
    return -1;
}

sh_script_handle sh_script_hooks_record(uint32_t spawn_id, const char *const *names, int count, int *full)
{
    char *chain;
    size_t used = 0, total = 1;
    int i, mod;
    sh_script_handle h;
    if (full) *full = 0;
    mod = sh_script_hooks_owner_of(names, count);
    if (mod < 0) return 0;
    for (i = 0; i < count; i++) total += (names[i] ? strlen(names[i]) : 0) + 1;
    chain = (char *)malloc(total);
    if (!chain) { if (full) *full = 1; return 0; }
    chain[0] = 0;
    for (i = 0; i < count; i++) {
        size_t n = names[i] ? strlen(names[i]) : 0, k;
        if (!n) continue;
        if (used) chain[used++] = '\n';
        for (k = 0; k < n; k++) chain[used + k] = (char)lower((unsigned char)names[i][k]);
        used += n;
        chain[used] = 0;
    }
    h = sh_script_handles_add_named(mod, spawn_id, chain, full);
    free(chain);
    return h;
}

/* ---- decisions ------------------------------------------------------------ */

int sh_script_tick_decide(const sh_script_tick_state *s)
{
    if (!s || s->open <= 0) return SH_SCRIPT_TICK_IDLE;
    if (s->in_tick) return SH_SCRIPT_TICK_REENTERED;
    if (s->on_main != 1) return s->on_main == 0 ? SH_SCRIPT_TICK_OFF_MAIN : SH_SCRIPT_TICK_UNKNOWN;
    if (s->is_client == 1) return SH_SCRIPT_TICK_CLIENT;
    if (s->is_client != 0) return SH_SCRIPT_TICK_UNKNOWN;
    if (s->loading == 1) return s->started ? SH_SCRIPT_TICK_LEFT : SH_SCRIPT_TICK_LOADING;
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
    case SH_SCRIPT_TICK_LEFT: return "left the map";
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

int sh_script_gate_opens(int gate)
{
    return gate == SH_SCRIPT_GATE_PLAY;
}

static const char *gate_name(int gate)
{
    return gate == SH_SCRIPT_GATE_PLAY ? "Play build" : gate == SH_SCRIPT_GATE_MAP_LOAD ? "map load" : "gate";
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

/* Without the engine there is no Play: nothing is open and nothing reloads,
 * unless a test says a Play runs. */
static int g_test_play;

void sh_script_hooks_test_play(int on)
{
    g_test_play = on != 0;
}

int sh_script_hooks_open(void)
{
    return g_test_play;
}

int sh_script_hooks_reload(void)
{
    int n;
    if (!g_test_play) return 0;
    sh_script_map_close(0);
    n = sh_script_map_open();
    return n > 0 ? n : 0;
}

#else /* the engine side ------------------------------------------------- */

#include "../backend_log.h"
#include "../feature_health.h"
#include "../entity.h"
#include "../hook.h"
#include "../engine_globals.h"
#include "../companion_engine.h"
#include "script_api_ai.h"
#include "../../fault_shield/load_screen_stop.h"
#include "script_source.h"
#include "script_api_common.h"
#include "script_api_combat.h"
#include "script_api_reflect.h"
#include "script_api_game.h"

#define LOAD_SCREEN_RUNNING_BYTE 0x4B2   /* idLoadScreen thread: 1 while it draws (load_screen_stop.c) */
#define EDITOR_ACTIVE_BYTE       0x09    /* idSnapEditorLocal: set while editing, clear during every SnapMap Play */
#define CHAIN_MAX                9       /* an entityDef and its inherits in the mods namespace */

typedef void (*tick_fn)(void *superscript);
typedef unsigned char (*is_client_fn)(void *gm);
typedef void *(*lookup_fn)(void *gm, const uint32_t *spawn_id);

static tick_fn       g_orig_tick;
static void         *g_tick_target;
static is_client_fn  g_is_client;
static void         *g_lookup;           /* idGameLocal vtable +0x7F0, checked against the live vtable */
static const uint32_t *g_main_tid;
static const uint8_t *g_load_screen;     /* idLoadScreen; its first field is the thread object */
static const uint8_t *g_editor;          /* the idSnapEditorLocal singleton (glb "editor_singleton") */
static int           g_resolved;

static int           g_open;             /* states this map opened */
static int           g_started;          /* the runtime had a tick this map */
static int           g_in_tick;
static int           g_close_pending;    /* CancelMapChange during the tick: close after it */
static int           g_close_pending_handlers;
static uint32_t      g_frame;
static volatile LONG g_routing;          /* records are accepted for this map's states */
static volatile LONG g_full_logged;
static char *g_log_last;   /* the last line logged, malloc'd */
static int           g_vtable_checked;   /* 1 verified, -1 refused */
static double        g_time;             /* seconds of script frames this Play */
static LARGE_INTEGER g_last_frame;       /* the previous script frame's game milliseconds + 1 */

static void commit_combat_hooks(void);

static void hooks_log(const char *fmt, ...)
{
    char *line;
    va_list ap;
    va_start(ap, fmt);
    line = sh_vtextf(fmt, ap);
    va_end(ap);
    if (!line) return;
    /* the same line again says nothing new */
    if (g_log_last && !strcmp(line, g_log_last)) { free(line); return; }
    backend_log(line);
    free(g_log_last);
    g_log_last = line;
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

/* 1 during SnapMap Play of any kind (the editor is inactive), 0 while the
 * editor is active, -1 unknown. */
static int editor_inactive(void)
{
    uint8_t active;
    if (!g_editor) return -1;
    __try { active = *(const volatile uint8_t *)(g_editor + EDITOR_ACTIVE_BYTE); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    return active ? 0 : 1;
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
    sh_ai_spawn_teleport_reset();   /* a map's scripts name their spawn teleports again */
    g_open = 0;
    g_started = 0;
    g_frame = 0;
    g_time = 0;
    g_last_frame.QuadPart = 0;
    g_close_pending = 0;
}

static int prologue_ok(void)
{
    int same = 0;
    __try { same = !memcmp(g_tick_target, k_script_tick_prologue, SH_SCRIPT_TICK_STOLEN); }
    __except (EXCEPTION_EXECUTE_HANDLER) { same = 0; }
    return same;
}

/* The frame time from the game clock (idGameLocal vtable +0x220, the
 * scripts' game time, script_api_common.h): the game milliseconds since the
 * previous script frame (0 for the first). g_last_frame holds the previous
 * frame's game milliseconds plus one (0: none yet). An unreadable clock is
 * said once and script time stands still for that frame. */
static double frame_seconds(void)
{
    static volatile LONG logged;
    int ms = 0;
    double dt = 0;
    if (!sh_script_api_now_ms(&ms)) {
        if (!InterlockedExchange(&logged, 1))
            hooks_log("Mod scripts: the game clock could not be read, so script time does not advance this frame.");
        return 0;
    }
    if (g_last_frame.QuadPart && (LONGLONG)ms + 1 >= g_last_frame.QuadPart)
        dt = (double)((LONGLONG)ms + 1 - g_last_frame.QuadPart) / 1000.0;
    g_last_frame.QuadPart = (LONGLONG)ms + 1;
    g_time += dt;
    return dt;
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
    s.in_play = editor_inactive();
    s.started = g_started;
    d = sh_script_tick_decide(&s);
    switch (d) {
    case SH_SCRIPT_TICK_RUN: {
        double dt;
        g_in_tick = 1;
        if (!g_started) {
            int walked = 0;
            __try { walked = sh_script_hooks_walk_live(); }
            __except (av_only(GetExceptionCode())) {
                hooks_log("Mod scripts: a fault while reading the map's entities was contained; entities the spawn "
                          "hook did not see are not driven by their scripts.");
            }
            if (walked) hooks_log("SCRIPTS: %d placed entities recorded from the live entity list", walked);
        }
        g_started = 1;
        g_frame++;
        dt = frame_seconds();
        sh_script_api_frame();
        __try {
            sh_script_tick(g_time, dt);
        } __except (av_only(GetExceptionCode())) {
            hooks_log("Mod scripts stopped for this map after an internal fault in Snapmap+; the game goes on. "
                      "Please report it with this log.");
            g_close_pending = 1;
            g_close_pending_handlers = 0;
        }
        g_in_tick = 0;
        if (g_close_pending) map_close(g_close_pending_handlers);
        break;
    }
    case SH_SCRIPT_TICK_END:
        map_close(1);
        break;
    case SH_SCRIPT_TICK_LEFT:
        map_close(0);
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

void sh_script_hooks_gate(int gate)
{
    int n;
    if (!g_resolved) return;
    if (on_main() != 1) return;   /* both gates are main-thread gates; another caller is ignored */
    if (g_in_tick) return;        /* a gate cannot run inside the frame; never close under the runtime */
    map_close(0);                 /* the departed map's entities are gone at a map gate */
    InterlockedExchange(&g_full_logged, 0);
    free(g_log_last);
    g_log_last = NULL;            /* a new map may say again what the last one said */
    sh_script_handles_reset();
    sh_ai_spawn_teleport_reset();   /* a map's scripts name their spawn teleports again */
    if (!sh_script_gate_opens(gate)) return;
    n = sh_script_map_open();
    if (n <= 0) {
        if (n < 0) hooks_log("Mod scripts do not run in this map: its mods could not be read.");
        sh_script_map_close(0);
        mods_clear();
        return;
    }
    if (!commit_tick(gate_name(gate))) {
        sh_script_map_close(0);
        mods_clear();
        return;
    }
    commit_combat_hooks();
    sh_script_combat_reset();
    g_open = n;
    g_started = 0;
    g_time = 0;
    g_last_frame.QuadPart = 0;
    InterlockedExchange(&g_routing, 1);
    hooks_log("SCRIPTS: %d mod(s) with scripts prepared at the %s", n, gate_name(gate));
}

int sh_script_hooks_open(void)
{
    return g_open > 0;
}

int sh_script_hooks_reload(void)
{
    int n;
    if (!g_resolved || !g_open || g_in_tick || on_main() != 1) return 0;
    InterlockedExchange(&g_routing, 0);
    sh_script_map_close(g_started);
    mods_clear();
    sh_script_handles_reset();
    sh_ai_spawn_teleport_reset();   /* a map's scripts name their spawn teleports again */
    g_open = 0;
    g_started = 0;
    g_time = 0;
    g_last_frame.QuadPart = 0;
    sh_script_combat_reset();
    n = sh_script_map_open();
    if (n <= 0) {
        sh_script_map_close(0);
        mods_clear();
        return 0;
    }
    g_open = n;
    InterlockedExchange(&g_routing, 1);
    return n;
}

int sh_script_hooks_walk_live(void)
{
    void *gm = sh_entity_gamemgr();
    int num, added = 0;
    if (!g_open || !gm || !vtable_ok()) return 0;
    for (num = 0; num < SH_SCRIPT_ENT_NUMBER_WORLD; num++) {
        const char *names[CHAIN_MAX];
        uint32_t id;
        void *ent;
        int n, full = 0;
        id = *(const uint32_t *)((const uint8_t *)gm + SH_SCRIPT_GAME_SPAWN_IDS + (size_t)num * 4);
        if (id == SH_SCRIPT_SPAWN_ID_NONE || id == SH_SCRIPT_SPAWN_ID_WORLD) continue;
        if (sh_script_handles_find(id, NULL)) continue;
        ent = resolve_spawn_id(id);
        if (!ent) continue;
        n = sh_companions_def_chain(ent, names, CHAIN_MAX);
        if (n <= 0) continue;
        if (sh_script_hooks_record(id, names, n, &full)) added++;
        else if (full) break;
    }
    return added;
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
    if (!sh_script_hooks_record(id, names, count, &full) && full && !InterlockedExchange(&g_full_logged, 1))
        hooks_log("Mod scripts: there was not enough memory to record an entity of %s, so it is not driven by its "
                  "scripts.", sh_script_hooks_mod_name(mod) ? sh_script_hooks_mod_name(mod) : "a mod");
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

int sh_script_hooks_on_game_thread(void) { return on_main() == 1; }

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

/* ---- damage, death, pain, animation notify and projectile hooks ----------
 *
 * Six detours, committed with the frame hook at the first Play with scripts
 * and never unhooked. Each calls the engine's own function, then posts one
 * record to the queue of the mod whose entity it concerns (script_queue.h):
 * no Lua runs inside them, and they may run on AI prethink and animWeb job
 * threads. On a client the first tick closes the scripts, so nothing they
 * post is ever delivered there.
 *   OnDamage dispatcher  (entity, attacker spawn id, amount): every entity class's damage taken; the
 *                        amount is what the engine applied. When an idAI2 applies it, the record its
 *                        Damage_Apply holds (the per-thread context below) adds the damage decl, the
 *                        damage zone of the first hit and the direction.
 *   Damage_Apply         (ai, record): sets the context, calls through, clears it.
 *   idAI2::Killed        (ai, inflictor, attacker, parms, ...): the death, once per entity.
 *   SetPainReaction      (ai, reaction, ms): a pain reaction started; reported with the decl and attacker
 *                        of the entity's last hit.
 *   OnAnimNotify         (entity, message): an md6 row ae_notify "<message>" ran.
 *   idProjectile::Launch (projectile, owner, origin, axis, ...): a projectile an entity of a scripted
 *                        mod launched (its weapon, ai:fire, an md6 row). */

typedef void (*on_damage_fn)(void *entity, uint32_t attacker_spawn_id, float amount);
typedef void (*damage_apply_fn)(void *ai, void *record);
typedef void (*killed_fn)(void *ai, void *inflictor, void *attacker, void *parms, uint64_t a5, void *a6, void *a7,
                          uint64_t a8);
typedef void (*set_pain_fn)(void *ai, int reaction, int ms);
typedef void (*anim_notify_fn)(void *entity, const char *message);
typedef void (*launch_fn)(void *projectile, void *owner, const float *origin, const float *axis, void *a5,
                          uint64_t a6, uint64_t a7, uint64_t a8);

typedef struct combat_hook {
    const char *what;            /* for the log line */
    void       *target;
    void       *detour;
    size_t      stolen;
    uint8_t     bytes[48];   /* the window as resolved (hook_prepare takes at most 48) */
    void       *tramp;
} combat_hook;

enum { CH_ON_DAMAGE, CH_DAMAGE_APPLY, CH_KILLED, CH_SET_PAIN, CH_ANIM_NOTIFY, CH_LAUNCH, CH_COUNT };
static combat_hook g_ch[CH_COUNT];
static int         g_ch_committed;
static DWORD       g_damage_tls = TLS_OUT_OF_INDEXES;

typedef struct damage_ctx { void *ai; const uint8_t *record; struct damage_ctx *outer; } damage_ctx;

/* "mods/<salt>/x" of mod `mod` as "x" (a pointer into full); any other name as it is. */
static const char *relative_name(int mod, const char *full)
{
    const char *salt = sh_script_hooks_mod_salt(mod);
    size_t n = salt ? strlen(salt) : 0;
    if (!full) return "";
    if (n && !_strnicmp(full, "mods/", 5) && !_strnicmp(full + 5, salt, n) && full[5 + n] == '/' && full[6 + n])
        full += 6 + n;
    return full;
}

/* A whole copy of an engine string (malloc'd), or NULL when it is unreadable. */
static char *engine_dup(const char *s)
{
    size_t n = 0;
    char *out;
    if (!s) return NULL;
    __try { n = strlen(s); } __except (av_only(GetExceptionCode())) { return NULL; }
    out = (char *)malloc(n + 1);
    if (!out) return NULL;
    __try { memcpy(out, s, n); out[n] = 0; } __except (av_only(GetExceptionCode())) { free(out); return NULL; }
    return out;
}

/* The name of the decl at `decl` (idDecl +0x08), malloc'd; NULL when unreadable. */
static char *decl_name(const void *decl)
{
    return engine_dup((const char *)read_ptr(decl, SH_COMBAT_DECL_NAME));
}

/* A handle a script can hold for an entity, by spawn id (0 for none or the world). */
static sh_script_handle handle_for_id(uint32_t id)
{
    sh_script_handle h;
    if (id == SH_SCRIPT_SPAWN_ID_NONE || id == SH_SCRIPT_SPAWN_ID_WORLD) return 0;
    h = sh_script_handles_find(id, NULL);
    return h ? h : sh_script_handles_ref(id);
}

static sh_script_handle handle_for(void *entity)
{
    return entity ? handle_for_id(spawn_id_of(entity)) : 0;
}

static void on_damage_detour(void *entity, uint32_t attacker_id, float amount)
{
    sh_script_event ev;
    damage_ctx *ctx;
    char *decl = NULL, *zone = NULL;
    int mod = -1;
    sh_script_handle self;
    ((on_damage_fn)g_ch[CH_ON_DAMAGE].tramp)(entity, attacker_id, amount);
    self = sh_script_hooks_handle_of(entity, &mod);
    if (!self || mod < 0) return;
    memset(&ev, 0, sizeof ev);
    ev.kind = SH_SCRIPT_EV_DAMAGE;
    ev.self = self;
    ev.other = handle_for_id(attacker_id);
    ev.amount = amount;
    ctx = g_damage_tls != TLS_OUT_OF_INDEXES ? (damage_ctx *)TlsGetValue(g_damage_tls) : NULL;
    if (ctx && ctx->ai == entity) {
        const uint8_t *rec = ctx->record;
        const char *zone_at = NULL;
        void *decl_at = NULL;
        __try {
            int hits = *(const int *)(rec + SH_COMBAT_REC_HITS), i;
            ev.third = handle_for(*(void * const *)(rec + SH_COMBAT_REC_INFLICTOR));
            decl_at = *(void * const *)(rec + SH_COMBAT_REC_DECL);
            memcpy(ev.vec, rec + SH_COMBAT_REC_DIR, 3 * sizeof(float));
            ev.flags |= SH_SCRIPT_EVF_VEC;
            /* the engine's own record: at most SH_COMBAT_REC_ZONES_MAX hits are laid out in it */
            if (hits > SH_COMBAT_REC_ZONES_MAX) hits = SH_COMBAT_REC_ZONES_MAX;
            for (i = 0; i < hits && !zone_at; i++) {
                const char *const *z = *(const char *const *const *)(rec + SH_COMBAT_REC_ZONE + (size_t)i * 8);
                if (z && *z && **z) zone_at = *z;
            }
        } __except (av_only(GetExceptionCode())) {
            ev.flags = 0;
            decl_at = NULL;
            zone_at = NULL;
        }
        decl = decl_name(decl_at);
        zone = engine_dup(zone_at);
        ev.text = relative_name(mod, decl);
        ev.text2 = zone;
    }
    sh_script_combat_note_hit(self, ev.text, ev.other);
    sh_script_queue_push(mod, &ev);
    free(decl);
    free(zone);
}

static void damage_apply_detour(void *ai, void *record)
{
    damage_ctx ctx;
    int tls = g_damage_tls != TLS_OUT_OF_INDEXES;
    ctx.ai = ai;
    ctx.record = (const uint8_t *)record;
    ctx.outer = tls ? (damage_ctx *)TlsGetValue(g_damage_tls) : NULL;
    if (tls) TlsSetValue(g_damage_tls, &ctx);
    ((damage_apply_fn)g_ch[CH_DAMAGE_APPLY].tramp)(ai, record);
    if (tls) TlsSetValue(g_damage_tls, ctx.outer);
}

static void killed_detour(void *ai, void *inflictor, void *attacker, void *parms, uint64_t a5, void *a6, void *a7,
                          uint64_t a8)
{
    sh_script_event ev;
    char *name = NULL;
    int mod = -1;
    sh_script_handle self = sh_script_hooks_handle_of(ai, &mod);
    memset(&ev, 0, sizeof ev);
    if (self && mod >= 0) {
        ev.kind = SH_SCRIPT_EV_DEATH;
        ev.self = self;
        ev.other = handle_for(attacker);
        ev.third = handle_for(inflictor);
        name = decl_name(read_ptr(parms, SH_COMBAT_PARMS_DECL));
        ev.text = relative_name(mod, name);
    }
    ((killed_fn)g_ch[CH_KILLED].tramp)(ai, inflictor, attacker, parms, a5, a6, a7, a8);
    if (self && mod >= 0 && sh_script_combat_first_death(self)) sh_script_queue_push(mod, &ev);
    free(name);
}

static void set_pain_detour(void *ai, int reaction, int ms)
{
    sh_script_event ev;
    const char *name;
    char *decl = NULL;
    int mod = -1;
    sh_script_handle self;
    ((set_pain_fn)g_ch[CH_SET_PAIN].tramp)(ai, reaction, ms);
    name = sh_script_combat_pain_name(reaction);
    if (!name || reaction == SH_COMBAT_PAIN_NONE) return;
    self = sh_script_hooks_handle_of(ai, &mod);
    if (!self || mod < 0) return;
    memset(&ev, 0, sizeof ev);
    ev.kind = SH_SCRIPT_EV_PAIN;
    ev.self = self;
    ev.text = name;
    sh_script_combat_last_hit(self, &decl, &ev.other);
    ev.text2 = decl;
    sh_script_queue_push(mod, &ev);
    free(decl);
}

static void anim_notify_detour(void *entity, const char *message)
{
    int mod = -1;
    sh_script_handle self;
    char *text;
    ((anim_notify_fn)g_ch[CH_ANIM_NOTIFY].tramp)(entity, message);
    self = sh_script_hooks_handle_of(entity, &mod);
    if (!self || mod < 0 || !message) return;
    text = engine_dup(message);   /* the row's whole text */
    if (text && text[0]) sh_script_queue_post(mod, SH_SCRIPT_EV_ANIM, self, 0, 0, 0.0f, text, NULL);
    free(text);
}

static void launch_detour(void *projectile, void *owner, const float *origin, const float *axis, void *a5,
                          uint64_t a6, uint64_t a7, uint64_t a8)
{
    sh_script_event ev;
    char *name;
    int mod = -1;
    uint32_t id;
    sh_script_handle self;
    float from[3], heading[9];
    self = owner ? sh_script_hooks_handle_of(owner, &mod) : 0;
    /* a script's ai:fire from a tag: the launch starts there (the engine's event gave the origin) */
    if (self && mod >= 0 && sh_script_combat_launch_from(self, from, heading)) { origin = from; axis = heading; }
    ((launch_fn)g_ch[CH_LAUNCH].tramp)(projectile, owner, origin, axis, a5, a6, a7, a8);
    if (!self || mod < 0) return;
    id = spawn_id_of(projectile);
    if (id == SH_SCRIPT_SPAWN_ID_NONE) return;
    memset(&ev, 0, sizeof ev);
    ev.kind = SH_SCRIPT_EV_PROJECTILE;
    ev.self = self;
    ev.other = sh_script_combat_launch(id, self, mod);   /* its handle and launch record, released together */
    name = decl_name(read_ptr(projectile, SH_COMBAT_PROJ_DECL));
    ev.text = relative_name(mod, name);
    __try {
        if (origin && axis) {
            memcpy(ev.vec, origin, 3 * sizeof(float));
            memcpy(ev.vec + 3, axis, 3 * sizeof(float));   /* the axis' first row: the launch direction */
            ev.flags |= SH_SCRIPT_EVF_VEC;
        }
    } __except (av_only(GetExceptionCode())) { ev.flags = 0; }
    if (ev.other) sh_script_queue_push(mod, &ev);
    free(name);
}

/* Record a resolved target and its window bytes (install, main thread). */
static void combat_hook_set(int which, const char *what, uintptr_t addr, void *detour, size_t stolen)
{
    combat_hook *c = &g_ch[which];
    c->what = what;
    c->target = (void *)addr;
    c->detour = detour;
    c->stolen = stolen;
    if (addr) memcpy(c->bytes, (const void *)addr, stolen);
}

/* Commit every resolved combat hook (the Play gate, main thread, with the
 * frame hook). A hook that cannot be committed leaves its event unreported:
 * one line says which. */
static void commit_combat_hooks(void)
{
    int i, failed = 0;
    char missing[160];
    size_t used = 0;
    if (g_ch_committed) return;
    g_ch_committed = 1;
    missing[0] = 0;
    for (i = 0; i < CH_COUNT; i++) {
        combat_hook *c = &g_ch[i];
        int same = 0;
        void *tramp;
        if (!c->target) { failed = 1; goto note; }
        __try { same = !memcmp(c->target, c->bytes, c->stolen); } __except (EXCEPTION_EXECUTE_HANDLER) { same = 0; }
        if (!same) { failed = 1; goto note; }
        tramp = hook_prepare(c->target, c->detour, c->stolen);
        if (!tramp) { failed = 1; goto note; }
        c->tramp = tramp;
        if (hook_commit(tramp) != B2_PATCH_OK) {
            if (hook_unpatch(tramp)) c->tramp = NULL;
            failed = 1;
            goto note;
        }
        continue;
note:
        {
            int w = _snprintf_s(missing + used, sizeof missing - used, _TRUNCATE, "%s%s", used ? ", " : "", c->what);
            if (w > 0) used += (size_t)w;
        }
    }
    if (failed) {
        char why[220];
        hooks_log("Mod scripts: these game events cannot reach scripts in this session: %s. Other script calls work.",
                  missing);
        _snprintf_s(why, sizeof why, _TRUNCATE, "the hooks for %s could not be installed", missing);
        sh_feature_off(SH_FEATURE_SCRIPT_EVENTS, why);
    }
    else
        hooks_log("SCRIPTS: damage, death, pain, animation and projectile hooks installed");
}

/* Install: resolve the combat hooks' targets (commits nothing). */
static void resolve_combat_hooks(const sig_result *r, const uint8_t *module_base)
{
    sig_result s;
    if (g_damage_tls == TLS_OUT_OF_INDEXES) g_damage_tls = TlsAlloc();
    combat_hook_set(CH_ON_DAMAGE, "damage", (uintptr_t)own(r, SH_SCRIPT_SIG_ON_DAMAGE), (void *)on_damage_detour,
                    SH_SCRIPT_ON_DAMAGE_STOLEN);
    combat_hook_set(CH_KILLED, "death", (uintptr_t)own(r, SH_SCRIPT_SIG_KILLED), (void *)killed_detour,
                    SH_SCRIPT_KILLED_STOLEN);
    s.addr = 0;
    combat_hook_set(CH_ANIM_NOTIFY, "animation notify",
                    sh_script_resolve_named(module_base, &SH_SCRIPT_ANIM_NOTIFY_SIGNATURE, &s) == SIG_OK ? s.addr : 0,
                    (void *)anim_notify_detour, SH_SCRIPT_ON_ANIM_STOLEN);
    s.addr = 0;
    combat_hook_set(CH_DAMAGE_APPLY, "damage details",
                    sig_resolve_one(module_base, &SH_COMBAT_SIGNATURES[SH_COMBAT_SIG_DAMAGE_APPLY], &s) == SIG_OK ? s.addr : 0,
                    (void *)damage_apply_detour, SH_COMBAT_DAMAGE_APPLY_STOLEN);
    s.addr = 0;
    combat_hook_set(CH_SET_PAIN, "pain",
                    sig_resolve_one(module_base, &SH_COMBAT_SIGNATURES[SH_COMBAT_SIG_SET_PAIN], &s) == SIG_OK ? s.addr : 0,
                    (void *)set_pain_detour, SH_COMBAT_SET_PAIN_STOLEN);
    s.addr = 0;
    combat_hook_set(CH_LAUNCH, "projectile launch",
                    sig_resolve_one(module_base, &SH_COMBAT_SIGNATURES[SH_COMBAT_SIG_PROJECTILE_LAUNCH], &s) == SIG_OK ? s.addr : 0,
                    (void *)launch_detour, SH_COMBAT_LAUNCH_STOLEN);
    /* The boot check: every target found by its signature on this
     * executable; what is missing is a feature-registry row now, not a
     * surprise at the first Play. */
    {
        char missing[160], why[220];
        size_t used = 0;
        int i;
        missing[0] = 0;
        for (i = 0; i < CH_COUNT; i++) {
            int w;
            if (g_ch[i].target) continue;
            w = _snprintf_s(missing + used, sizeof missing - used, _TRUNCATE, "%s%s", used ? ", " : "", g_ch[i].what);
            if (w > 0) used += (size_t)w;
        }
        if (used) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "the game functions for %s were not found in this executable",
                        missing);
            sh_feature_off(SH_FEATURE_SCRIPT_EVENTS, why);
        }
    }
}


int sh_script_hooks_install(const sig_result *results, size_t count, const uint8_t *module_base)
{
    sig_result r[SH_SCRIPT_SIG_COUNT];
    int i;
    if (g_resolved) return 1;
    if (!module_base) return 0;
    for (i = 0; i < SH_SCRIPT_SIG_COUNT; i++) {
        if (sig_resolve_one(module_base, &SH_SCRIPT_SIGNATURES[i], &r[i]) != SIG_OK && i <= SH_SCRIPT_SIG_IS_CLIENT) {
            char why[160];
            _snprintf_s(why, sizeof why, _TRUNCATE, "the game function they need (%s) was not found in this "
                        "executable", SH_SCRIPT_SIGNATURES[i].name);
            sh_feature_off(SH_FEATURE_MOD_SCRIPTS, why);
            return 0;
        }
    }
    g_tick_target = own(r, SH_SCRIPT_SIG_TICK);
    g_lookup = own(r, SH_SCRIPT_SIG_SPAWN_ID_LOOKUP);
    g_is_client = (is_client_fn)own(r, SH_SCRIPT_SIG_IS_CLIENT);
    g_main_tid = (const uint32_t *)glb_resolve(module_base, "main_thread_id", NULL);
    g_load_screen = (const uint8_t *)glb_resolve(module_base, "load_screen", NULL);
    g_editor = (const uint8_t *)glb_resolve(module_base, "editor_singleton", NULL);
    if (!g_main_tid || !g_load_screen || !g_editor) {
        sh_feature_off(SH_FEATURE_MOD_SCRIPTS, !g_main_tid ? "the game's main thread was not found in this executable"
                       : !g_load_screen ? "the game's loading screen was not found in this executable"
                       : "the editor was not found in this executable");
        return 0;
    }
    if (!prologue_ok()) {
        sh_feature_off(SH_FEATURE_MOD_SCRIPTS, "the game's frame function is not the expected one (another tool "
                       "may have changed it)");
        return 0;
    }
    sh_script_handles_set_resolver(resolve_spawn_id);
    (void)sh_script_api_install(results, count, module_base);   /* logs its own line when it cannot */
    (void)sh_script_combat_install(results, count, module_base);   /* a missing part is its own registry row */
    (void)sh_script_api_reflect_install();
    (void)sh_script_api_game_install(results, count, module_base);
    resolve_combat_hooks(r, module_base);
    /* Spawns during Play reach scripts through the companions' CallSpawn
     * detour, resolved and checked at their install (before this one). */
    if (!sh_companions_spawn_ready())
        sh_feature_off(SH_FEATURE_SCRIPT_SPAWNS, "the game's spawn function is not the expected one in this executable, "
                       "so only entities placed in the map reach scripts");
    sh_script_source_install();
    sh_companions_set_spawn_subscriber(sh_script_hooks_after_spawn, sh_script_hooks_spawn_wanted);
    sh_load_screen_stop_set_map_end(sh_script_hooks_map_cancel);
    g_resolved = 1;
    backend_log("SCRIPTS: engine side resolved; the frame hook is installed at the first map with scripts");
    return 1;
}

#endif /* SH_SCRIPT_PURE */
