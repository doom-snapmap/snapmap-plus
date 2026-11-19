/* sync_clock.c -- the engine side of the sync clock (sync_clock.h).
 *
 * The re-anchor (sync_clock_sigs.c) is prepared at boot and committed at the first Play that serves a mod; with no
 * mod served no hook is committed. The detour passes every call through unchanged unless its precision differs from
 * the player's (the loop's own `precise` for an idAI2 under the cvar's default mode 1, read through the cvar's value
 * at idCVar +0x30, which is what the loop reads: VK 0x145B133E0, GL 0x144418A60). Only then is the animator looked
 * up in the set of animators on the player's clock, rebuilt once per pass of the loop (a pass is one `now` and one
 * rate): every player's idPlayer.mostRecentSyncEntity, followed by its spawn id; while that sync entity
 * isCurrentlyWorking, its activeParticipants give each participant (instance, a reference) and its role
 * (data->type); the rule (sync_clock_rule.c) keeps them when the TARGET is a mod's entity. Everything runs on the
 * main thread: the loop is called from idGameLocal's frame, the gates from the serving layer.
 *
 * Read live (R-LV5, Vulkan, the Tyrant's from-above kill): idPlayer.syncMaster stays empty during a glory kill;
 * mostRecentSyncEntity names the demon's sync entity (mods/<salt>/syncmelee/<demon>_N) from the kill's start, and
 * its isCurrentlyWorking is set for the kill's length and cleared at its end. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "sync_clock.h"
#include "modserve.h"
#include "feature_health.h"
#include "backend_log.h"
#include "hook.h"
#include "patch.h"
#include "engine_globals.h"
#include "engine_build.h"
#include "engine_cvar_read.h"
#include "script/script_reflect.h"
#include "script/script_api_reflect.h"
#include "script/script_api_common.h"
#include "script/script_handles.h"
#include "../common/text.h"

#define SC_PRECISION_CVAR  "g_fixupForAdaptiveTick_PrecisionMode"
#define SC_VSLOT_ANIMATOR  0x948u   /* idEntity vtable: the animator the re-anchor loop re-anchors */
#define SC_PLAYER_SLOTS    8        /* entity numbers below this hold the players (SnapMap Play has at most 4) */
#define SC_DEF_MAX         192

typedef void (*reanchor_fn)(void *animator, const int *now, int hz, int old_hz, char precise, char is_ai);
typedef void *(*animator_fn)(void *entity);

/* Where the fields live in this session, placed through the game's type tables. */
typedef struct sc_layout {
    uint32_t player_sync;     /* idPlayer.mostRecentSyncEntity: an idManagedClassPtr, its spawn id at +0 */
    uint32_t working;         /* idSyncEntity.isCurrentlyWorking: a bool */
    uint32_t participants;    /* idSyncEntity.activeParticipants: an idList (elements +0, count +8) */
    uint32_t part_size;       /* one idActiveSyncParticipant */
    uint32_t part_instance;   /* idActiveSyncParticipant.instance: an idManagedClassPtr */
    uint32_t part_data;       /* idActiveSyncParticipant.data: an idSyncParticipant * */
    uint32_t data_type;       /* idSyncParticipant.type: a participantType_t */
    int32_t  target;          /* the TARGET member of that enum */
} sc_layout;

static void *g_tramp;
static reanchor_fn g_reanchor;
static int g_ready;                 /* the re-anchor resolved and its detour prepared */
static int g_committed;
static int g_placed;                /* g_layout holds this session's offsets */
static sc_layout g_layout;
static const void *g_cvar_slot;
static void *g_cvar;                /* the precision cvar, found at the first Play gate */
static volatile LONG g_live;        /* a Play runs and the fields are placed */

/* The set of this pass. */
static int g_pass_now, g_pass_hz, g_pass_valid;
static const void *g_set[SH_SC_PARTICIPANTS_MAX];
static int g_nset;
static uint32_t g_sync_id = SH_SCRIPT_SPAWN_ID_NONE;   /* the mod sync the set comes from, or NONE */

/* What the console lists (this Play). */
static volatile LONG g_syncs, g_moved;
static char g_last_sync[SC_DEF_MAX], g_last_target[SC_DEF_MAX];
static int g_last_count;

static void sc_log(const char *fmt, ...)
{
    char *line;
    va_list ap;
    va_start(ap, fmt);
    line = sh_vtextf(fmt, ap);
    va_end(ap);
    if (line) backend_log(line);
    free(line);
}

static int av_only(DWORD code)
{
    return code == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}

/* ----------------------------------------------------------------- reads */

static int sc_mode(void)
{
    int mode = -1;
    if (!g_cvar || !sh_engine_cvar_read_int(g_cvar, &mode)) return -1;
    return mode;
}

static int rd_u32(const void *at, uint32_t *out)
{
    __try { *out = *(const uint32_t *)at; }
    __except (av_only(GetExceptionCode())) { return 0; }
    return 1;
}

static const void *rd_ptr(const void *at)
{
    const void *p = NULL;
    __try { p = *(const void *const *)at; }
    __except (av_only(GetExceptionCode())) { p = NULL; }
    return p;
}

static int rd_byte(const void *at)
{
    int v = -1;
    __try { v = *(const uint8_t *)at; }
    __except (av_only(GetExceptionCode())) { v = -1; }
    return v;
}

static void *sc_animator(void *entity)
{
    void *a = NULL;
    __try {
        animator_fn f = (animator_fn)(*(void *const *const *)entity)[SC_VSLOT_ANIMATOR / sizeof(void *)];
        a = f ? f(entity) : NULL;
    } __except (av_only(GetExceptionCode())) { a = NULL; }
    return a;
}

/* One working sync entity's participants into p (their entityDef names in defs); the count. */
static int sc_read_sync(const sh_script_engine *e, const uint8_t *sync, sh_sc_participant *p,
                        char defs[][SC_DEF_MAX])
{
    const uint8_t *list = sync + g_layout.participants, *elems;
    uint32_t num = 0, i;
    int n = 0;
    if (!(elems = (const uint8_t *)rd_ptr(list)) || !rd_u32(list + SH_RF_LIST_NUM, &num)) return 0;
    for (i = 0; i < num && n < SH_SC_PARTICIPANTS_MAX; i++) {
        const uint8_t *el = elems + (size_t)i * g_layout.part_size, *data;
        uint32_t id = SH_SCRIPT_SPAWN_ID_NONE, type = 0;
        void *who;
        if (!rd_u32(el + g_layout.part_instance + SH_RF_REF_SPAWN_ID, &id) || !(who = e->resolve(id))) continue;
        data = (const uint8_t *)rd_ptr(el + g_layout.part_data);
        p[n].target = data && rd_u32(data + g_layout.data_type, &type) && (int32_t)type == g_layout.target;
        defs[n][0] = '\0';
        p[n].def = e->def_name && e->def_name(who, defs[n], SC_DEF_MAX) ? defs[n] : NULL;
        p[n].animator = sc_animator(who);
        n++;
    }
    return n;
}

/* The set of this pass: the animators of the first mod sync a player is in, none when there is none. */
static void sc_refresh(void)
{
    const sh_script_engine *e = sh_script_api_engine();
    sh_sc_participant p[SH_SC_PARTICIPANTS_MAX];
    static char defs[SH_SC_PARTICIPANTS_MAX][SC_DEF_MAX];
    uint32_t found = SH_SCRIPT_SPAWN_ID_NONE;
    int slot, n = 0, i;
    g_nset = 0;
    if (!e || !e->resolve || !e->slot_spawn_id || !e->kind) return;
    for (slot = 0; slot < SC_PLAYER_SLOTS && !g_nset; slot++) {
        void *player = e->resolve(e->slot_spawn_id(slot));
        uint32_t id = SH_SCRIPT_SPAWN_ID_NONE;
        const uint8_t *sync;
        if (!player || e->kind(player) != SH_SCRIPT_KIND_PLAYER) continue;
        if (!rd_u32((const uint8_t *)player + g_layout.player_sync + SH_RF_REF_SPAWN_ID, &id)) continue;
        if (!(sync = (const uint8_t *)e->resolve(id)) || rd_byte(sync + g_layout.working) != 1) continue;
        n = sc_read_sync(e, sync, p, defs);
        g_nset = sh_sc_player_clock(p, n, g_set, SH_SC_PARTICIPANTS_MAX);
        if (g_nset) found = id;
        if (g_nset && found != g_sync_id) {
            /* a mod sync began: one more for the listing */
            InterlockedIncrement(&g_syncs);
            g_last_count = g_nset;
            if (!e->def_name || !e->def_name((void *)sync, g_last_sync, sizeof g_last_sync)) g_last_sync[0] = '\0';
            g_last_target[0] = '\0';
            for (i = 0; i < n; i++)
                if (p[i].target && p[i].def) strncpy_s(g_last_target, sizeof g_last_target, p[i].def, _TRUNCATE);
#ifdef SH_DIAG
            sc_log("sync-clock: %s (target %s): %d participant animator(s) re-anchor on the player's clock",
                   g_last_sync[0] ? g_last_sync : "a sync", g_last_target[0] ? g_last_target : "?", g_nset);
#endif
        }
    }
    g_sync_id = found;
}

static int sc_on_player_clock(const void *animator, const int *now, int hz)
{
    int t = 0, i;
    if (!animator) return 0;
    __try { t = now ? *now : 0; }
    __except (av_only(GetExceptionCode())) { t = 0; }
    if (!g_pass_valid || t != g_pass_now || hz != g_pass_hz) {
        g_pass_now = t;
        g_pass_hz = hz;
        g_pass_valid = 1;
        sc_refresh();
    }
    for (i = 0; i < g_nset; i++) if (g_set[i] == animator) return 1;
    return 0;
}

/* --------------------------------------------------------------- detour */

static void reanchor_detour(void *animator, const int *now, int hz, int old_hz, char precise, char is_ai)
{
    if (precise && g_live) {
        int mode = sc_mode();
        /* the player re-anchors precisely only in mode 2: below it, a precise call is an AI's */
        if (mode >= 0 && mode != SH_SC_MODE_PRECISE_ALL && sc_on_player_clock(animator, now, hz)) {
            precise = (char)sh_sc_precision(precise, 1, mode);
            InterlockedIncrement(&g_moved);
        }
    }
    g_reanchor(animator, now, hz, old_hz, precise, is_ai);
}

/* --------------------------------------------------------------- install */

int sh_sync_clock_install(const uint8_t *module_base)
{
    sig_result own;
    if (g_ready) return 1;
    if (sig_resolve_one(module_base, &SH_SYNC_CLOCK_SIGNATURES[0], &own) != SIG_OK || !own.addr) {
        sh_feature_off(SH_FEATURE_SYNC_CLOCK, "the game function it needs was not found in this executable");
        sc_log("MODS: sync-clock: unavailable: %s was not found in this executable (%s)",
               SH_SYNC_CLOCK_SIGNATURES[0].name, sig_status_reason(own.status));
        return 0;
    }
    g_tramp = hook_prepare_relative_jcc((void *)own.addr, (void *)reanchor_detour, SH_SC_STOLEN, SH_SC_JCC_OFFSET);
    if (!g_tramp) {
        sc_log("MODS: sync-clock: unavailable: %s could not be detoured (%s)", SH_SYNC_CLOCK_SIGNATURES[0].name,
               hook_last_failure());
        sh_feature_off(SH_FEATURE_SYNC_CLOCK, "the game function it needs could not be detoured");
        return 0;
    }
    g_reanchor = (reanchor_fn)g_tramp;
    g_cvar_slot = (const void *)glb_resolve(module_base, "cvar_system_slot", NULL);
    g_ready = 1;
    sc_log("MODS: sync-clock: the animation re-anchor resolved; detoured at the first Play that serves a mod");
    return 1;
}

static int sc_commit(void)
{
    if (g_committed) return 1;
    if (!g_ready) return 0;
    if (hook_commit(g_tramp) != B2_PATCH_OK) {
        sc_log("MODS: sync-clock: the animation re-anchor could not be detoured (the patch was refused); a mod "
               "demon's kills play on two clocks");
        g_ready = 0;
        sh_feature_off(SH_FEATURE_SYNC_CLOCK, "the game function it needs could not be detoured");
        return 0;
    }
    g_committed = 1;
    sc_log("MODS: sync-clock: the animation re-anchor is detoured");
    return 1;
}

/* ------------------------------------------------------------ the layout */

/* The offset of cls.name, which must classify as `kind`; its classified place in *loc when asked (freed by the
 * caller). 0 with why. */
static int sc_field(const sh_rf_host *h, const char *cls, const char *name, int kind, uint32_t *off,
                    sh_rf_loc *loc, const char **why)
{
    sh_rf_field f;
    sh_rf_loc l;
    if (!sh_rf_find_field(h, cls, name, &f)) { *why = name; return 0; }
    if (!sh_rf_classify(h, f.type, f.ops, f.size, NULL, &l)) { *why = "memory"; return 0; }
    if (l.kind != kind) { sh_rf_loc_free(&l); *why = name; return 0; }
    *off = f.offset;
    if (loc) *loc = l;
    else sh_rf_loc_free(&l);
    return 1;
}

static int sc_place(const char **why)
{
    const sh_rf_host *h = sh_script_reflect_host();
    sh_rf_loc list, data, type;
    sc_layout L;
    long long target = -1;
    int ok;
    if (!h) { *why = "the game's type tables could not be read"; return 0; }
    memset(&L, 0, sizeof L);
    if (!sc_field(h, "idPlayer", "mostRecentSyncEntity", SH_RF_REF, &L.player_sync, NULL, why) ||
        !sc_field(h, "idSyncEntity", "isCurrentlyWorking", SH_RF_BOOL, &L.working, NULL, why) ||
        !sc_field(h, "idSyncEntity", "activeParticipants", SH_RF_LIST, &L.participants, &list, why))
        return 0;
    ok = list.type && h->class_size && h->class_size(h->ctx, list.type, &L.part_size) && L.part_size &&
         sc_field(h, list.type, "instance", SH_RF_REF, &L.part_instance, NULL, why) &&
         sc_field(h, list.type, "data", SH_RF_POINTER, &L.part_data, &data, why);
    sh_rf_loc_free(&list);
    if (!ok) return 0;
    ok = data.type && sc_field(h, data.type, "type", SH_RF_ENUM, &L.data_type, &type, why);
    sh_rf_loc_free(&data);
    if (!ok) return 0;
    ok = type.type && h->enum_value && h->enum_value(h->ctx, type.type, "TARGET", &target);
    sh_rf_loc_free(&type);
    if (!ok) { *why = "TARGET"; return 0; }
    L.target = (int32_t)target;
    g_layout = L;
    return 1;
}

/* ---------------------------------------------------------- the gates */

typedef struct sc_served { int any; } sc_served;

static void sc_served_one(void *context, const char *id, const char *hash, const char *registries)
{
    sh_modserve_scope_info info;
    (void)id;
    (void)registries;
    if (hash && sh_modserve_bundle_scope(hash, &info) && info.names_live) ((sc_served *)context)->any = 1;
}

void sh_sync_clock_play_begin(void)
{
    sc_served s;
    const char *why = NULL;
    InterlockedExchange(&g_live, 0);
    g_pass_valid = 0;
    g_nset = 0;
    g_sync_id = SH_SCRIPT_SPAWN_ID_NONE;
    InterlockedExchange(&g_syncs, 0);
    InterlockedExchange(&g_moved, 0);
    g_last_sync[0] = g_last_target[0] = '\0';
    g_last_count = 0;
    if (!g_ready || sh_feature_is_off(SH_FEATURE_SYNC_CLOCK)) return;
    s.any = 0;
    (void)sh_modserve_each_bundle_registries(sc_served_one, &s);
    if (!s.any) return;
    if (!g_cvar && g_cvar_slot) g_cvar = sh_engine_cvar_find(g_cvar_slot, SH_IDCVAR_NAME_FOR_LOOKUP, SC_PRECISION_CVAR);
    if (!g_cvar) {
        sc_log("MODS: sync-clock: unavailable: the cvar %s was not found; a mod demon's kills play on two clocks",
               SC_PRECISION_CVAR);
        sh_feature_off(SH_FEATURE_SYNC_CLOCK, "a cvar it reads was not found");
        return;
    }
    if (!g_placed) {
        if (!sc_place(&why)) {
            sc_log("MODS: sync-clock: unavailable: the game's type tables have no %s where it is expected; a mod "
                   "demon's kills play on two clocks", why ? why : "field");
            sh_feature_off(SH_FEATURE_SYNC_CLOCK, "a field it reads was not found in the game's type tables");
            return;
        }
        g_placed = 1;
    }
    if (!sc_commit()) return;
    InterlockedExchange(&g_live, 1);
}

void sh_sync_clock_map_end(void)
{
    InterlockedExchange(&g_live, 0);
    g_pass_valid = 0;
    g_nset = 0;
    g_sync_id = SH_SCRIPT_SPAWN_ID_NONE;
}

/* ------------------------------------------------------------ the console */

void sh_sync_clock_command(sh_sc_line_fn line, void *context)
{
    char *text;
    if (!line) return;
    text = sh_textf("MODS: sync-clock: %s; precision mode %d (the player re-anchors precisely only in mode 2)",
                    sh_feature_is_off(SH_FEATURE_SYNC_CLOCK) ? "unavailable in this session"
                    : g_live ? "the animation re-anchor is detoured; this Play serves mods"
                    : g_committed ? "the animation re-anchor is detoured; no Play that serves mods runs"
                    : "detoured at the first Play that serves a mod", sc_mode());
    if (text) line(context, text);
    free(text);
    text = g_syncs ? sh_textf("  this Play: %ld mod demon sync(s); %ld re-anchor(s) on the player's clock; the last: "
                              "%s (target %s), %d participant animator(s)", (long)g_syncs, (long)g_moved,
                              g_last_sync[0] ? g_last_sync : "?", g_last_target[0] ? g_last_target : "?", g_last_count)
                   : sh_textf("  this Play: no mod demon's sync yet");
    if (text) line(context, text);
    free(text);
}
