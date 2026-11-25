/* clip_events.c -- the engine side of clip events (clip_events.h). The three event lookups are prepared at boot and
 * committed at the first Play whose mods register a clip; each detour calls the engine first and, when it finds
 * nothing for a registered clip on a model in its carrier's chain, calls the same lookup on the carrier (not while the
 * editor is active). The carrier is found by name at the clip's first lookup in a Play (loaded on the main thread
 * through sh_modserve_engine_decl, find-only elsewhere through sh_modserve_engine_decl_held), kept for that Play and
 * checked by name before each use. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "clip_events.h"
#include "md6_registry_engine.h"
#include "feature_health.h"
#include "backend_log.h"
#include "hook.h"
#include "patch.h"
#include "script/script_hooks.h"
#include "../common/text.h"

#define ANIM_NAME     0x08u    /* idMD6Anim: its resource name (the key of the event tables) */
#define MODEL_NAME    0x08u    /* idDeclMD6: its decl name */
#define LIST_COUNT    0x08u

typedef unsigned char (*win_fn)(void *model, void *anim, uint32_t from, uint32_t to, void *out);
typedef unsigned char (*type_fn)(void *model, void *anim, uint32_t from, uint32_t to, uint32_t filter, void *out);

static void *g_tramp[SH_CE_SITE_COUNT];
static win_fn  g_win;
static type_fn g_type, g_type2;
static int g_ready;                         /* the three lookups resolved and their detours prepared */
static int g_committed;
static sh_md6_runtime g_registry;


/* Returns the carrier whose rows answer this lookup, or NULL: the clip's name is under md6/mods/, it is
 * registered, a Play runs, and `model` is in the carrier's chain. */
static void *ce_carrier(void *model, void *anim, sh_md6_row **out)
{
    const char *name;
    sh_md6_registry *reg;
    sh_md6_row *row = NULL;
    void *c;
    if (!model || !anim) return NULL;
    name = sh_md6_name_at(anim, ANIM_NAME);
    __try {
        if (!name || !sh_ce_under_mods(name)) return NULL;
        reg = (sh_md6_registry *)g_registry.current;
        row = reg ? sh_md6_registry_find(reg, name) : NULL;
    } __except (sh_md6_av_only(GetExceptionCode())) { row = NULL; }
    if (!row || sh_script_hooks_in_play() == 0) return NULL;
    c = sh_md6_model(row, SH_MD6_CLIP_EVENTS);
    if (!c || !sh_ce_in_chain(c, model, sh_md6_parent, NULL)) return NULL;
    *out = row;
    return c;
}

static void ce_answered(sh_md6_row *row, void *model, void *out)
{
    InterlockedIncrement(&row->answered);
#ifdef SH_DIAG
    if (!InterlockedExchange(&row->logged, 1)) {
        int n = -1;
        const char *on = sh_md6_name_at(model, MODEL_NAME);
        __try { n = *(const int *)((const uint8_t *)out + LIST_COUNT); } __except (sh_md6_av_only(GetExceptionCode())) { n = -1; }
        __try { sh_md6_log("clip-events: \"%s\" on %s: %d rows from \"%s\"", row->name, on ? on : "?", n, row->carrier); }
        __except (sh_md6_av_only(GetExceptionCode())) { (void)0; }
    }
#else
    (void)model;
    (void)out;
#endif
}

static unsigned char win_detour(void *model, void *anim, uint32_t from, uint32_t to, void *out)
{
    unsigned char r = g_win(model, anim, from, to, out);
    sh_md6_row *row = NULL;
    void *c;
    if (r || !g_registry.any) return r;
    if (!(c = ce_carrier(model, anim, &row))) return r;
    r = g_win(c, anim, from, to, out);
    if (r) ce_answered(row, model, out);
    return r;
}

static unsigned char type_detour(void *model, void *anim, uint32_t from, uint32_t to, uint32_t filter, void *out)
{
    unsigned char r = g_type(model, anim, from, to, filter, out);
    sh_md6_row *row = NULL;
    void *c;
    if (r || !g_registry.any) return r;
    if (!(c = ce_carrier(model, anim, &row))) return r;
    r = g_type(c, anim, from, to, filter, out);
    if (r) ce_answered(row, model, out);
    return r;
}

static unsigned char type2_detour(void *model, void *anim, uint32_t from, uint32_t to, uint32_t filter, void *out)
{
    unsigned char r = g_type2(model, anim, from, to, filter, out);
    sh_md6_row *row = NULL;
    void *c;
    if (r || !g_registry.any) return r;
    if (!(c = ce_carrier(model, anim, &row))) return r;
    r = g_type2(c, anim, from, to, filter, out);
    if (r) ce_answered(row, model, out);
    return r;
}

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
            sh_md6_log("MODS: clip-events: unavailable: %s (%s)", why ? why : "a lookup did not resolve",
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
            sh_md6_log("MODS: clip-events: unavailable: %s could not be detoured (%s)", SH_CE_SITES[i].fn.name,
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
    sh_md6_log("MODS: clip-events: the three event lookups resolved; detoured at the first Play whose mods add rows to "
           "their clips");
    return 1;
}

/* Commits all three detours or none, so a clip's rows never apply to only some lookups. */
static int ce_commit(void)
{
    int i;
    if (g_committed) return 1;
    if (!g_ready) return 0;
    for (i = 0; i < SH_CE_SITE_COUNT; i++) {
        if (hook_commit(g_tramp[i]) == B2_PATCH_OK) continue;
        sh_md6_log("MODS: clip-events: %s could not be detoured (the patch was refused); clips play without the events "
               "their mods add", SH_CE_SITES[i].fn.name);
        while (--i >= 0) (void)hook_unpatch(g_tramp[i]);
        g_ready = 0;
        sh_feature_off(SH_FEATURE_CLIP_EVENTS, "a game function they need could not be detoured");
        return 0;
    }
    g_committed = 1;
    sh_md6_log("MODS: clip-events: the event lookups are detoured");
    return 1;
}

void sh_clip_events_play_begin(void)
{
    int oom = 0, i, k;
    sh_md6_registry *r = sh_md6_registry_build(SH_MD6_CLIP_EVENTS, &oom);
    if (oom) sh_md6_log("MODS: clip-events: out of memory while reading the mods' animation_events; some clips play without "
                    "the events their mods add");
    if (!r || !r->n) {
        if (r) { sh_md6_registry_free(r); free(r); }
        sh_md6_registry_publish(&g_registry, NULL);
        return;
    }
    if (sh_feature_is_off(SH_FEATURE_CLIP_EVENTS) || !g_ready || !ce_commit()) {
        /* One line per mod and Play. */
        for (i = 0; i < r->n; i++) {
            for (k = 0; k < i && strcmp(r->rows[k].salt, r->rows[i].salt); k++) {}
            if (k == i)
                sh_md6_log("MODS: clip-events: %s: its clips play without the events it adds (animation_events): Clip events is "
                       "unavailable in this session.", r->rows[i].label);
        }
        sh_md6_registry_free(r);
        free(r);
        sh_md6_registry_publish(&g_registry, NULL);
        return;
    }
    sh_md6_registry_publish(&g_registry, r);
    sh_md6_log("MODS: clip-events: %d clip(s) of %d mod(s) get event rows from their own md6Defs in this Play", r->n,
           r->mods);
}

void sh_clip_events_map_end(void)
{
    if (g_registry.current) sh_md6_registry_publish(&g_registry, NULL);
}

void sh_clip_events_command(sh_ce_line_fn line, void *context)
{
    sh_md6_registry *reg = (sh_md6_registry *)g_registry.current, *preview = NULL;
    int oom = 0;
    const char *state;
    char *text;
    if (!line) return;
    if (!reg) reg = preview = sh_md6_registry_build(SH_MD6_CLIP_EVENTS, &oom);
    if (!reg || !reg->n) {
        line(context, oom ? "MODS: clip-events: out of memory" :
             "MODS: clip-events: the open map's mods add no event rows to their clips (no mod.json \"animation_events\" "
             "in the builds it serves)");
        if (preview) { sh_md6_registry_free(preview); free(preview); }
        return;
    }
    state = sh_feature_is_off(SH_FEATURE_CLIP_EVENTS) ? "Clip events is unavailable in this session"
          : g_committed ? "the event lookups are detoured"
          : "the event lookups are detoured at the first Play that registers a clip";
    text = sh_textf("MODS: clip-events: %d clip(s) of %d mod(s) %s; %s", reg->n, reg->mods,
                    preview ? "in the open map's mods, registered at its next Play" : "registered for this Play", state);
    if (text) line(context, text);
    free(text);
    sh_md6_registry_print(reg, SH_MD6_CLIP_EVENTS, line, context);
    if (preview) { sh_md6_registry_free(preview); free(preview); }
}
