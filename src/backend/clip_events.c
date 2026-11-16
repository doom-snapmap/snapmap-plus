/* clip_events.c -- the engine side of clip events (clip_events.h).
 *
 * The three event lookups (clip_events_sigs.c) are prepared at boot and committed at the first Play whose mods
 * register a clip; with no registration no hook is committed. Each detour calls the engine first. A lookup the
 * model's own chain answers returns that answer; a lookup it does not answer returns unless some clip is
 * registered and the clip's name is under md6/mods/ (the one test every stock clip leaves at). A registered
 * clip on a model in its carrier's chain (sh_ce_in_chain) gets a second call of the same engine lookup on the
 * carrier, the mod's md6Def: the engine walks the carrier and its parents and fills the list itself. Nothing
 * of a game model is written.
 *
 * The carrier is found by name the first time one of its clips is looked up in a Play: on the main thread
 * through the engine's own find, which loads a served name on demand at the engine's level, as an entity
 * naming it would (sh_modserve_engine_decl); on another thread find only (sh_modserve_engine_decl_held), and
 * a carrier not loaded yet is asked for again at the next lookup. The pointer is kept only for that Play: the
 * registry is built at the Play gate from names and dropped at the next gate, and the pointer is checked by
 * its name before each use. A carrier is never kept across a purge: loaded after the Play's purge, it binds to
 * the models that Play loaded and is freed with them. Lookups while the editor is active are left to the
 * engine. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "clip_events.h"
#include "modserve.h"
#include "feature_health.h"
#include "backend_log.h"
#include "hook.h"
#include "patch.h"
#include "script/script_hooks.h"
#include "../common/text.h"

#define ANIM_NAME     0x08u    /* idMD6Anim: its resource name (the key of the event tables) */
#define MODEL_NAME    0x08u    /* idDeclMD6: its decl name */
#define MODEL_PARENT  0x98u    /* idDeclMD6: the md6Def it inherits */
#define LIST_COUNT    0x08u    /* idList: the number of rows */
#define CARRIER_TYPE  "md6Def"

typedef unsigned char (*win_fn)(void *model, void *anim, uint32_t from, uint32_t to, void *out);
typedef unsigned char (*type_fn)(void *model, void *anim, uint32_t from, uint32_t to, uint32_t filter, void *out);

static void *g_tramp[SH_CE_SITE_COUNT];
static win_fn  g_win;
static type_fn g_type, g_type2;
static int g_ready;                         /* the three lookups resolved and their detours prepared */
static int g_committed;
static sh_ce_registry *volatile g_reg;      /* this Play's registry, or NULL */
static sh_ce_registry *g_retired;           /* the one before it, freed at the next swap */
static volatile LONG g_any;

static void ce_log(const char *fmt, ...)
{
    char *line;
    va_list ap;
    va_start(ap, fmt);
    line = sh_vtextf(fmt, ap);
    va_end(ap);
    if (line) backend_log(line);
    free(line);
}

static void ce_line_log(void *context, const char *line)
{
    (void)context;
    backend_log(line);
}

static int av_only(DWORD code)
{
    return code == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}

/* ----------------------------------------------------------------- reads */

static int ce_parent(void *context, const void *model, const void **parent)
{
    (void)context;
    __try { *parent = *(const void *const *)((const uint8_t *)model + MODEL_PARENT); }
    __except (av_only(GetExceptionCode())) { *parent = NULL; return 0; }
    return 1;
}

static const char *ce_name_at(const void *object, size_t offset)
{
    const char *name = NULL;
    __try { name = *(const char *const *)((const uint8_t *)object + offset); }
    __except (av_only(GetExceptionCode())) { name = NULL; }
    return name;
}

/* 1 when the object at `model` is still the md6Def `name` (it is checked before each use). */
static int ce_still(const void *model, const char *name)
{
    int same = 0;
    __try { same = sh_ce_name_eq(*(const char *const *)((const uint8_t *)model + MODEL_NAME), name); }
    __except (av_only(GetExceptionCode())) { same = 0; }
    return same;
}

/* The carrier of a row in this Play, found by name, or NULL. */
static void *ce_model(sh_ce_row *row)
{
    void *c = row->model;
    if (row->state == SH_CE_ROW_MISSING) return NULL;
    if (c && ce_still(c, row->carrier)) return c;
    row->model = NULL;
    if (sh_modserve_engine_on_main() == 1) {
        c = sh_modserve_engine_decl(CARRIER_TYPE, row->carrier);
        if (!c) {
            if (InterlockedExchange(&row->state, SH_CE_ROW_MISSING) != SH_CE_ROW_MISSING)
                ce_log("MODS: clip-events: %s: \"%s\" did not load, so \"%s\" plays without the events it adds. Open "
                       "the Mods tab: the mod's warnings, or the backend log line before this one, say what to fix.",
                       row->label, row->carrier, row->clip);
            return NULL;
        }
    } else {
        void *o = NULL;
        unsigned state = 0;
        if (sh_modserve_engine_decl_held(CARRIER_TYPE, row->carrier, &o, &state, NULL) != 1 || (state & 3))
            return NULL;   /* not loaded yet: the main thread's next lookup loads it */
        c = o;
    }
    row->model = c;
    InterlockedExchange(&row->state, SH_CE_ROW_READY);
    return c;
}

/* The carrier whose rows answer this lookup, or NULL: the clip's name is under md6/mods/, it is registered,
 * a Play runs, and `model` is in the carrier's chain. */
static void *ce_carrier(void *model, void *anim, sh_ce_row **out)
{
    const char *name;
    sh_ce_registry *reg;
    sh_ce_row *row = NULL;
    void *c;
    if (!model || !anim) return NULL;
    name = ce_name_at(anim, ANIM_NAME);
    __try {
        if (!name || !sh_ce_under_mods(name)) return NULL;
        reg = (sh_ce_registry *)g_reg;
        row = reg ? sh_ce_registry_find(reg, name) : NULL;
    } __except (av_only(GetExceptionCode())) { row = NULL; }
    if (!row || sh_script_hooks_in_play() == 0) return NULL;
    c = ce_model(row);
    if (!c || !sh_ce_in_chain(c, model, ce_parent, NULL)) return NULL;
    *out = row;
    return c;
}

static void ce_answered(sh_ce_row *row, void *model, void *out)
{
    InterlockedIncrement(&row->answered);
#ifdef SH_DIAG
    if (!InterlockedExchange(&row->logged, 1)) {
        int n = -1;
        const char *on = ce_name_at(model, MODEL_NAME);
        __try { n = *(const int *)((const uint8_t *)out + LIST_COUNT); } __except (av_only(GetExceptionCode())) { n = -1; }
        __try { ce_log("clip-events: \"%s\" on %s: %d rows from \"%s\"", row->clip, on ? on : "?", n, row->carrier); }
        __except (av_only(GetExceptionCode())) { (void)0; }
    }
#else
    (void)model;
    (void)out;
#endif
}

/* --------------------------------------------------------------- detours */

static unsigned char win_detour(void *model, void *anim, uint32_t from, uint32_t to, void *out)
{
    unsigned char r = g_win(model, anim, from, to, out);
    sh_ce_row *row = NULL;
    void *c;
    if (r || !g_any) return r;
    if (!(c = ce_carrier(model, anim, &row))) return r;
    r = g_win(c, anim, from, to, out);
    if (r) ce_answered(row, model, out);
    return r;
}

static unsigned char type_detour(void *model, void *anim, uint32_t from, uint32_t to, uint32_t filter, void *out)
{
    unsigned char r = g_type(model, anim, from, to, filter, out);
    sh_ce_row *row = NULL;
    void *c;
    if (r || !g_any) return r;
    if (!(c = ce_carrier(model, anim, &row))) return r;
    r = g_type(c, anim, from, to, filter, out);
    if (r) ce_answered(row, model, out);
    return r;
}

static unsigned char type2_detour(void *model, void *anim, uint32_t from, uint32_t to, uint32_t filter, void *out)
{
    unsigned char r = g_type2(model, anim, from, to, filter, out);
    sh_ce_row *row = NULL;
    void *c;
    if (r || !g_any) return r;
    if (!(c = ce_carrier(model, anim, &row))) return r;
    r = g_type2(c, anim, from, to, filter, out);
    if (r) ce_answered(row, model, out);
    return r;
}

/* --------------------------------------------------------------- install */

int sh_clip_events_install(const uint8_t *module_base)
{
    static void *const detours[SH_CE_SITE_COUNT] = { (void *)win_detour, (void *)type_detour, (void *)type2_detour };
    void *target[SH_CE_SITE_COUNT];
    int i;
    if (g_ready) return 1;
    for (i = 0; i < SH_CE_SITE_COUNT; i++) {
        sig_result r;
        if (sh_ce_site_resolve(module_base, &SH_CE_SITES[i], &r) != SIG_OK || !r.addr) {
            char *why = sh_textf("the game function they need (%s) was not found in this executable",
                                 SH_CE_SITES[i].fn.name);
            sh_feature_off(SH_FEATURE_CLIP_EVENTS, why ? why : "a game function they need was not found");
            ce_log("MODS: clip-events: unavailable: %s (%s)", why ? why : "a lookup did not resolve",
                   sig_status_reason(r.status));
            free(why);
            return 0;
        }
        target[i] = (void *)r.addr;
    }
    for (i = 0; i < SH_CE_SITE_COUNT; i++) {
        g_tramp[i] = hook_prepare(target[i], detours[i], SH_CE_SITES[i].stolen);
        if (!g_tramp[i]) {
            int k;
            ce_log("MODS: clip-events: unavailable: %s could not be detoured (%s)", SH_CE_SITES[i].fn.name,
                   hook_last_failure());
            for (k = 0; k < i; k++) if (hook_unpatch(g_tramp[k])) g_tramp[k] = NULL;
            sh_feature_off(SH_FEATURE_CLIP_EVENTS, "a game function they need could not be detoured");
            return 0;
        }
    }
    g_win = (win_fn)g_tramp[SH_CE_SITE_WINDOW];
    g_type = (type_fn)g_tramp[SH_CE_SITE_TYPE];
    g_type2 = (type_fn)g_tramp[SH_CE_SITE_TYPE2];
    g_ready = 1;
    ce_log("MODS: clip-events: the three event lookups resolved; detoured at the first Play whose mods add rows to "
           "their clips");
    return 1;
}

/* All three or none: a lookup left undetoured would play a clip's rows in some events and not others. */
static int ce_commit(void)
{
    int i;
    if (g_committed) return 1;
    if (!g_ready) return 0;
    for (i = 0; i < SH_CE_SITE_COUNT; i++) {
        if (hook_commit(g_tramp[i]) == B2_PATCH_OK) continue;
        ce_log("MODS: clip-events: %s could not be detoured (the patch was refused); clips play without the events "
               "their mods add", SH_CE_SITES[i].fn.name);
        while (--i >= 0) (void)hook_unpatch(g_tramp[i]);
        g_ready = 0;
        sh_feature_off(SH_FEATURE_CLIP_EVENTS, "a game function they need could not be detoured");
        return 0;
    }
    g_committed = 1;
    ce_log("MODS: clip-events: the event lookups are detoured");
    return 1;
}

/* ---------------------------------------------------------- the registry */

typedef struct ce_build { sh_ce_registry *r; int oom; } ce_build;

static void ce_add_bundle(void *context, const char *id, const char *hash, const char *registries)
{
    ce_build *b = (ce_build *)context;
    sh_modserve_scope_info info;
    char *label;
    if (!registries || !strstr(registries, "\"" SH_CE_REGISTRY_KEY "\"") || !hash) return;
    /* only the builds the open map serves now */
    if (!sh_modserve_bundle_scope(hash, &info) || !info.names_live) return;
    label = sh_modserve_bundle_label(hash);
    if (sh_ce_registry_add(b->r, id, label ? label : id, registries, ce_line_log, NULL) < 0) b->oom = 1;
    free(label);
}

static sh_ce_registry *ce_build_registry(int *oom)
{
    ce_build b;
    b.oom = 0;
    b.r = (sh_ce_registry *)calloc(1, sizeof *b.r);
    if (!b.r) { *oom = 1; return NULL; }
    (void)sh_modserve_each_bundle_registries(ce_add_bundle, &b);
    sh_ce_registry_seal(b.r);
    *oom = b.oom;
    return b.r;
}

static void ce_publish(sh_ce_registry *r)
{
    sh_ce_registry *old;
    InterlockedExchange(&g_any, 0);
    old = (sh_ce_registry *)InterlockedExchangePointer((void *volatile *)&g_reg, r);
    /* a lookup still reading the one before last has long returned: two gates lie between */
    if (g_retired) { sh_ce_registry_free(g_retired); free(g_retired); }
    g_retired = old;
    if (r && r->n) InterlockedExchange(&g_any, 1);
}

void sh_clip_events_play_begin(void)
{
    int oom = 0, i, k;
    sh_ce_registry *r = ce_build_registry(&oom);
    if (oom) ce_log("MODS: clip-events: out of memory while reading the mods' clip_events; some clips play without "
                    "the events their mods add");
    if (!r || !r->n) {
        if (r) { sh_ce_registry_free(r); free(r); }
        ce_publish(NULL);
        return;
    }
    if (sh_feature_is_off(SH_FEATURE_CLIP_EVENTS) || !g_ready || !ce_commit()) {
        /* one line per mod and Play */
        for (i = 0; i < r->n; i++) {
            for (k = 0; k < i && strcmp(r->rows[k].salt, r->rows[i].salt); k++) {}
            if (k == i)
                ce_log("MODS: clip-events: %s: its clips play without the events it adds (clip_events): Clip events is "
                       "unavailable in this session.", r->rows[i].label);
        }
        sh_ce_registry_free(r);
        free(r);
        ce_publish(NULL);
        return;
    }
    ce_publish(r);
    ce_log("MODS: clip-events: %d clip(s) of %d mod(s) get event rows from their own md6Defs in this Play", r->n,
           r->mods);
}

void sh_clip_events_map_end(void)
{
    if (g_reg) ce_publish(NULL);
}

/* ------------------------------------------------------------ the console */

void sh_clip_events_command(sh_ce_line_fn line, void *context)
{
    sh_ce_registry *reg = (sh_ce_registry *)g_reg, *preview = NULL;
    int oom = 0, i, k;
    const char *state;
    char *text;
    if (!line) return;
    if (!reg) reg = preview = ce_build_registry(&oom);
    if (!reg || !reg->n) {
        line(context, oom ? "MODS: clip-events: out of memory" :
             "MODS: clip-events: the open map's mods add no event rows to their clips (no mod.json \"clip_events\" "
             "in the builds it serves)");
        if (preview) { sh_ce_registry_free(preview); free(preview); }
        return;
    }
    state = sh_feature_is_off(SH_FEATURE_CLIP_EVENTS) ? "Clip events is unavailable in this session"
          : g_committed ? "the event lookups are detoured"
          : "the event lookups are detoured at the first Play that registers a clip";
    text = sh_textf("MODS: clip-events: %d clip(s) of %d mod(s) %s; %s", reg->n, reg->mods,
                    preview ? "in the open map's mods, registered at its next Play" : "registered for this Play", state);
    if (text) line(context, text);
    free(text);
    for (i = 0; i < reg->n; i++) {
        const sh_ce_row *w = &reg->rows[i];
        void *obj = NULL;
        unsigned st = 0, level = 0;
        int held;
        for (k = 0; k < i && !sh_ce_name_eq(reg->rows[k].carrier, w->carrier); k++) {}
        if (k < i) continue;   /* each carrier once, with its clips */
        held = sh_modserve_engine_decl_held(CARRIER_TYPE, w->carrier, &obj, &st, &level);
        text = sh_textf("  %s: \"%s\" over \"%s\": %s", w->label, w->carrier, w->base,
                        w->state == SH_CE_ROW_MISSING ? "did not load in this Play"
                        : held == 1 && !(st & 3) ? (level == 4 ? "loaded (kept for the session)" : "loaded")
                        : held == 1 ? "loaded as a default (its file was refused)"
                        : held == 0 ? "not loaded (it loads at the first lookup of one of its clips in Play)"
                        : "unknown (the engine's lists could not be read)");
        if (text) line(context, text);
        free(text);
        for (k = i; k < reg->n; k++) {
            const sh_ce_row *c = &reg->rows[k];
            if (!sh_ce_name_eq(c->carrier, w->carrier)) continue;
            text = c->answered ? sh_textf("    \"%s\": answered %ld lookup(s)", c->clip, (long)c->answered)
                               : sh_textf("    \"%s\"", c->clip);
            if (text) line(context, text);
            free(text);
        }
    }
    if (preview) { sh_ce_registry_free(preview); free(preview); }
}
