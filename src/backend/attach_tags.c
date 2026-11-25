/* attach_tags.c -- the engine side of attachment tags (attach_tags.h): a detour on the tag-info lookup returns
 * the engine's answer when it is a tag (int16 at +0x1C not -1); otherwise, for a registered mods/ prop whose
 * host model (props - 0x210) is in the carrier's chain, it asks again on the carrier's props table (carrier
 * + 0x210). The carrier is found by name on first use in a Play and checked by name before each use. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "attach_tags.h"
#include "modserve.h"
#include "feature_health.h"
#include "backend_log.h"
#include "hook.h"
#include "patch.h"
#include "script/script_hooks.h"
#include "../common/text.h"

#define MODEL_NAME     0x08u
#define MODEL_PARENT   0x98u    /* idDeclMD6: the md6Def it inherits */
#define MODEL_PROPS    0x210u   /* idDeclMD6: the props idList the tag-info lookup reads */
#define TAG_JOINT      0x1Cu    /* tag record: the resolved joint index (int16), -1 for no tag */
#define CARRIER_TYPE   "md6Def"

typedef const void *(*tag_fn)(const void *props, const char *prop, const char *tag);

static void *g_tramp;
static tag_fn g_lookup;
static int g_ready;                         /* the lookup resolved and its detour prepared */
static int g_committed;
static sh_at_registry *volatile g_reg;      /* this Play's registry, or NULL */
static sh_at_registry *g_retired;           /* the previous registry, freed at the next swap */
static volatile LONG g_any;

static void at_log(const char *fmt, ...)
{
    char *line;
    va_list ap;
    va_start(ap, fmt);
    line = sh_vtextf(fmt, ap);
    va_end(ap);
    if (line) backend_log(line);
    free(line);
}

static void at_line_log(void *context, const char *line)
{
    (void)context;
    backend_log(line);
}

static int av_only(DWORD code)
{
    return code == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}

static int at_parent(void *context, const void *model, const void **parent)
{
    (void)context;
    __try { *parent = *(const void *const *)((const uint8_t *)model + MODEL_PARENT); }
    __except (av_only(GetExceptionCode())) { *parent = NULL; return 0; }
    return 1;
}

static const char *at_name_at(const void *object, size_t offset)
{
    const char *name = NULL;
    __try { name = *(const char *const *)((const uint8_t *)object + offset); }
    __except (av_only(GetExceptionCode())) { name = NULL; }
    return name;
}

/* 1 when the object at `model` is still the md6Def `name` (it is checked before each use). */
static int at_still(const void *model, const char *name)
{
    int same = 0;
    __try { same = sh_ce_name_eq(*(const char *const *)((const uint8_t *)model + MODEL_NAME), name); }
    __except (av_only(GetExceptionCode())) { same = 0; }
    return same;
}

/* 1 when `r` is a tag: not NULL and its joint is not -1. An unreadable record is left to the engine (1). */
static int at_is_tag(const void *r)
{
    int tag = 1;
    if (!r) return 0;
    __try { tag = *(const int16_t *)((const uint8_t *)r + TAG_JOINT) != -1; }
    __except (av_only(GetExceptionCode())) { tag = 1; }
    return tag;
}

/* The carrier of a row in this Play, found by name, or NULL. */
static void *at_model(sh_at_row *row)
{
    void *c = row->model;
    if (row->state == SH_AT_ROW_MISSING) return NULL;
    if (c && at_still(c, row->carrier)) return c;
    row->model = NULL;
    if (sh_modserve_engine_on_main() == 1) {
        c = sh_modserve_engine_decl(CARRIER_TYPE, row->carrier);
        if (!c) {
            if (InterlockedExchange(&row->state, SH_AT_ROW_MISSING) != SH_AT_ROW_MISSING)
                at_log("MODS: attachment-tags: %s: \"%s\" did not load, so \"%s\" attaches without the tag info it "
                       "gives. Open the Mods tab: the mod's warnings, or the backend log line before this one, say "
                       "what to fix.", row->label, row->carrier, row->prop);
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
    InterlockedExchange(&row->state, SH_AT_ROW_READY);
    return c;
}

/* The carrier whose tag info answers this lookup, or NULL: the prop's name is under mods/, it is registered, a
 * Play runs, and the host model (props - 0x210) is in the carrier's chain. */
static void *at_carrier(const void *props, const char *prop, sh_at_row **out)
{
    sh_at_registry *reg;
    sh_at_row *row = NULL;
    void *c;
    if (!props || !prop) return NULL;
    __try {
        if (!sh_at_under_mods(prop)) return NULL;
        reg = (sh_at_registry *)g_reg;
        row = reg ? sh_at_registry_find(reg, prop) : NULL;
    } __except (av_only(GetExceptionCode())) { row = NULL; }
    if (!row || sh_script_hooks_in_play() == 0) return NULL;
    c = at_model(row);
    if (!c || !sh_ce_in_chain(c, (const uint8_t *)props - MODEL_PROPS, at_parent, NULL)) return NULL;
    *out = row;
    return c;
}

static void at_answered(sh_at_row *row, const void *props)
{
    InterlockedIncrement(&row->answered);
#ifdef SH_DIAG
    if (!InterlockedExchange(&row->logged, 1)) {
        const char *on = at_name_at((const uint8_t *)props - MODEL_PROPS, MODEL_NAME);
        __try { at_log("attachment-tags: \"%s\" on %s: tag info from \"%s\"", row->prop, on ? on : "?", row->carrier); }
        __except (av_only(GetExceptionCode())) { (void)0; }
    }
#else
    (void)props;
#endif
}

static const void *lookup_detour(const void *props, const char *prop, const char *tag)
{
    const void *r = g_lookup(props, prop, tag), *r2;
    sh_at_row *row = NULL;
    void *c;
    if (!g_any || at_is_tag(r)) return r;
    if (!(c = at_carrier(props, prop, &row))) return r;
    r2 = g_lookup((const uint8_t *)c + MODEL_PROPS, prop, tag);
    if (!r2 || !at_is_tag(r2)) return r;
    at_answered(row, props);
    return r2;
}

int sh_attach_tags_install(const uint8_t *module_base)
{
    sig_result own;
    if (g_ready) return 1;
    if (sig_resolve_one(module_base, &SH_ATTACH_TAGS_SIGNATURES[0], &own) != SIG_OK || !own.addr) {
        char *why = sh_textf("the game function they need (%s) was not found in this executable",
                             SH_ATTACH_TAGS_SIGNATURES[0].name);
        sh_feature_off(SH_FEATURE_ATTACHMENT_TAGS, why ? why : "a game function they need was not found");
        at_log("MODS: attachment-tags: unavailable: %s (%s)", why ? why : "the lookup did not resolve",
               sig_status_reason(own.status));
        free(why);
        return 0;
    }
    g_tramp = hook_prepare((void *)own.addr, (void *)lookup_detour, SH_AT_STOLEN);
    if (!g_tramp) {
        at_log("MODS: attachment-tags: unavailable: %s could not be detoured (%s)", SH_ATTACH_TAGS_SIGNATURES[0].name,
               hook_last_failure());
        sh_feature_off(SH_FEATURE_ATTACHMENT_TAGS, "a game function they need could not be detoured");
        return 0;
    }
    g_lookup = (tag_fn)g_tramp;
    g_ready = 1;
    at_log("MODS: attachment-tags: the tag-info lookup resolved; detoured at the first Play whose mods give tag info "
           "for their own attachments");
    return 1;
}

static int at_commit(void)
{
    if (g_committed) return 1;
    if (!g_ready) return 0;
    if (hook_commit(g_tramp) != B2_PATCH_OK) {
        at_log("MODS: attachment-tags: the tag-info lookup could not be detoured (the patch was refused); "
               "attachments get no tag info from their mods");
        g_ready = 0;
        sh_feature_off(SH_FEATURE_ATTACHMENT_TAGS, "a game function they need could not be detoured");
        return 0;
    }
    g_committed = 1;
    at_log("MODS: attachment-tags: the tag-info lookup is detoured");
    return 1;
}

typedef struct at_build { sh_at_registry *r; int oom; } at_build;

static void at_add_bundle(void *context, const char *id, const char *hash, const char *registries)
{
    at_build *b = (at_build *)context;
    sh_modserve_scope_info info;
    char *label;
    if (!registries || !strstr(registries, "\"" SH_AT_REGISTRY_KEY "\"") || !hash) return;
    /* only the builds the open map serves */
    if (!sh_modserve_bundle_scope(hash, &info) || !info.names_live) return;
    label = sh_modserve_bundle_label(hash);
    if (sh_at_registry_add(b->r, id, label ? label : id, registries, at_line_log, NULL) < 0) b->oom = 1;
    free(label);
}

static sh_at_registry *at_build_registry(int *oom)
{
    at_build b;
    b.oom = 0;
    b.r = (sh_at_registry *)calloc(1, sizeof *b.r);
    if (!b.r) { *oom = 1; return NULL; }
    (void)sh_modserve_each_bundle_registries(at_add_bundle, &b);
    sh_at_registry_seal(b.r);
    *oom = b.oom;
    return b.r;
}

static void at_publish(sh_at_registry *r)
{
    sh_at_registry *old;
    InterlockedExchange(&g_any, 0);
    old = (sh_at_registry *)InterlockedExchangePointer((void *volatile *)&g_reg, r);
    /* a lookup still reading the one before last has long returned: two gates lie between */
    if (g_retired) { sh_at_registry_free(g_retired); free(g_retired); }
    g_retired = old;
    if (r && r->n) InterlockedExchange(&g_any, 1);
}

void sh_attach_tags_play_begin(void)
{
    int oom = 0, i, k;
    sh_at_registry *r = at_build_registry(&oom);
    if (oom) at_log("MODS: attachment-tags: out of memory while reading the mods' attachment_tags; some attachments "
                    "get no tag info from their mods");
    if (!r || !r->n) {
        if (r) { sh_at_registry_free(r); free(r); }
        at_publish(NULL);
        return;
    }
    if (sh_feature_is_off(SH_FEATURE_ATTACHMENT_TAGS) || !g_ready || !at_commit()) {
        /* one line per mod and Play */
        for (i = 0; i < r->n; i++) {
            for (k = 0; k < i && strcmp(r->rows[k].salt, r->rows[i].salt); k++) {}
            if (k == i)
                at_log("MODS: attachment-tags: %s: its attachments get no tag info from it (attachment_tags): "
                       "Attachment tags is unavailable in this session.", r->rows[i].label);
        }
        sh_at_registry_free(r);
        free(r);
        at_publish(NULL);
        return;
    }
    at_publish(r);
    at_log("MODS: attachment-tags: %d prop(s) of %d mod(s) get tag info from their own md6Defs in this Play", r->n,
           r->mods);
}

void sh_attach_tags_map_end(void)
{
    if (g_reg) at_publish(NULL);
}

void sh_attach_tags_command(sh_ce_line_fn line, void *context)
{
    sh_at_registry *reg = (sh_at_registry *)g_reg, *preview = NULL;
    int oom = 0, i, k;
    const char *state;
    char *text;
    if (!line) return;
    if (!reg) reg = preview = at_build_registry(&oom);
    if (!reg || !reg->n) {
        line(context, oom ? "MODS: attachment-tags: out of memory" :
             "MODS: attachment-tags: the open map's mods give no tag info for their attachments (no mod.json "
             "\"attachment_tags\" in the builds it serves)");
        if (preview) { sh_at_registry_free(preview); free(preview); }
        return;
    }
    state = sh_feature_is_off(SH_FEATURE_ATTACHMENT_TAGS) ? "Attachment tags is unavailable in this session"
          : g_committed ? "the tag-info lookup is detoured"
          : "the tag-info lookup is detoured at the first Play that registers a prop";
    text = sh_textf("MODS: attachment-tags: %d prop(s) of %d mod(s) %s; %s", reg->n, reg->mods,
                    preview ? "in the open map's mods, registered at its next Play" : "registered for this Play", state);
    if (text) line(context, text);
    free(text);
    for (i = 0; i < reg->n; i++) {
        const sh_at_row *w = &reg->rows[i];
        void *obj = NULL;
        unsigned st = 0, level = 0;
        int held;
        for (k = 0; k < i && !sh_ce_name_eq(reg->rows[k].carrier, w->carrier); k++) {}
        if (k < i) continue;   /* each carrier once, with its props */
        held = sh_modserve_engine_decl_held(CARRIER_TYPE, w->carrier, &obj, &st, &level);
        text = sh_textf("  %s: \"%s\" over \"%s\": %s", w->label, w->carrier, w->base,
                        w->state == SH_AT_ROW_MISSING ? "did not load in this Play"
                        : held == 1 && !(st & 3) ? (level == 4 ? "loaded (kept for the session)" : "loaded")
                        : held == 1 ? "loaded as a default (its file was refused)"
                        : held == 0 ? "not loaded (it loads at the first lookup of one of its props in Play)"
                        : "unknown (the engine's lists could not be read)");
        if (text) line(context, text);
        free(text);
        for (k = i; k < reg->n; k++) {
            const sh_at_row *c = &reg->rows[k];
            if (!sh_ce_name_eq(c->carrier, w->carrier)) continue;
            text = c->answered ? sh_textf("    \"%s\": answered %ld lookup(s)", c->prop, (long)c->answered)
                               : sh_textf("    \"%s\"", c->prop);
            if (text) line(context, text);
            free(text);
        }
    }
    if (preview) { sh_at_registry_free(preview); free(preview); }
}
