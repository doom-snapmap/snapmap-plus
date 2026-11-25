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
#include "md6_registry_engine.h"
#include "feature_health.h"
#include "backend_log.h"
#include "hook.h"
#include "patch.h"
#include "script/script_hooks.h"
#include "../common/text.h"

#define MODEL_NAME     0x08u
#define MODEL_PROPS    0x210u   /* idDeclMD6: the props idList the tag-info lookup reads */
#define TAG_JOINT      0x1Cu    /* tag record: the resolved joint index (int16), -1 for no tag */

typedef const void *(*tag_fn)(const void *props, const char *prop, const char *tag);

static void *g_tramp;
static tag_fn g_lookup;
static int g_ready;                         /* the lookup resolved and its detour prepared */
static int g_committed;
static sh_md6_runtime g_registry;


/* 1 when `r` is a tag: not NULL and its joint is not -1. An unreadable record is left to the engine (1). */
static int at_is_tag(const void *r)
{
    int tag = 1;
    if (!r) return 0;
    __try { tag = *(const int16_t *)((const uint8_t *)r + TAG_JOINT) != -1; }
    __except (sh_md6_av_only(GetExceptionCode())) { tag = 1; }
    return tag;
}

/* The carrier whose tag info answers this lookup, or NULL: the prop's name is under mods/, it is registered, a
 * Play runs, and the host model (props - 0x210) is in the carrier's chain. */
static void *at_carrier(const void *props, const char *prop, sh_md6_row **out)
{
    sh_md6_registry *reg;
    sh_md6_row *row = NULL;
    void *c;
    if (!props || !prop) return NULL;
    __try {
        if (!sh_ce_decl_under_mods(prop)) return NULL;
        reg = (sh_md6_registry *)g_registry.current;
        row = reg ? sh_md6_registry_find(reg, prop) : NULL;
    } __except (sh_md6_av_only(GetExceptionCode())) { row = NULL; }
    if (!row || sh_script_hooks_in_play() == 0) return NULL;
    c = sh_md6_model(row, SH_MD6_ATTACHMENT_TAGS);
    if (!c || !sh_ce_in_chain(c, (const uint8_t *)props - MODEL_PROPS, sh_md6_parent, NULL)) return NULL;
    *out = row;
    return c;
}

static void at_answered(sh_md6_row *row, const void *props)
{
    InterlockedIncrement(&row->answered);
#ifdef SH_DIAG
    if (!InterlockedExchange(&row->logged, 1)) {
        const char *on = sh_md6_name_at((const uint8_t *)props - MODEL_PROPS, MODEL_NAME);
        __try { sh_md6_log("attachment-tags: \"%s\" on %s: tag info from \"%s\"", row->name, on ? on : "?", row->carrier); }
        __except (sh_md6_av_only(GetExceptionCode())) { (void)0; }
    }
#else
    (void)props;
#endif
}

static const void *lookup_detour(const void *props, const char *prop, const char *tag)
{
    const void *r = g_lookup(props, prop, tag), *r2;
    sh_md6_row *row = NULL;
    void *c;
    if (!g_registry.any || at_is_tag(r)) return r;
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
        sh_md6_log("MODS: attachment-tags: unavailable: %s (%s)", why ? why : "the lookup did not resolve",
               sig_status_reason(own.status));
        free(why);
        return 0;
    }
    g_tramp = hook_prepare((void *)own.addr, (void *)lookup_detour, SH_AT_STOLEN);
    if (!g_tramp) {
        sh_md6_log("MODS: attachment-tags: unavailable: %s could not be detoured (%s)", SH_ATTACH_TAGS_SIGNATURES[0].name,
               hook_last_failure());
        sh_feature_off(SH_FEATURE_ATTACHMENT_TAGS, "a game function they need could not be detoured");
        return 0;
    }
    g_lookup = (tag_fn)g_tramp;
    g_ready = 1;
    sh_md6_log("MODS: attachment-tags: the tag-info lookup resolved; detoured at the first Play whose mods give tag info "
           "for their own attachments");
    return 1;
}

static int at_commit(void)
{
    if (g_committed) return 1;
    if (!g_ready) return 0;
    if (hook_commit(g_tramp) != B2_PATCH_OK) {
        sh_md6_log("MODS: attachment-tags: the tag-info lookup could not be detoured (the patch was refused); "
               "attachments get no tag info from their mods");
        g_ready = 0;
        sh_feature_off(SH_FEATURE_ATTACHMENT_TAGS, "a game function they need could not be detoured");
        return 0;
    }
    g_committed = 1;
    sh_md6_log("MODS: attachment-tags: the tag-info lookup is detoured");
    return 1;
}

void sh_attach_tags_play_begin(void)
{
    int oom = 0, i, k;
    sh_md6_registry *r = sh_md6_registry_build(SH_MD6_ATTACHMENT_TAGS, &oom);
    if (oom) sh_md6_log("MODS: attachment-tags: out of memory while reading the mods' attachment_tags; some attachments "
                    "get no tag info from their mods");
    if (!r || !r->n) {
        if (r) { sh_md6_registry_free(r); free(r); }
        sh_md6_registry_publish(&g_registry, NULL);
        return;
    }
    if (sh_feature_is_off(SH_FEATURE_ATTACHMENT_TAGS) || !g_ready || !at_commit()) {
        /* one line per mod and Play */
        for (i = 0; i < r->n; i++) {
            for (k = 0; k < i && strcmp(r->rows[k].salt, r->rows[i].salt); k++) {}
            if (k == i)
                sh_md6_log("MODS: attachment-tags: %s: its attachments get no tag info from it (attachment_tags): "
                       "Attachment tags is unavailable in this session.", r->rows[i].label);
        }
        sh_md6_registry_free(r);
        free(r);
        sh_md6_registry_publish(&g_registry, NULL);
        return;
    }
    sh_md6_registry_publish(&g_registry, r);
    sh_md6_log("MODS: attachment-tags: %d prop(s) of %d mod(s) get tag info from their own md6Defs in this Play", r->n,
           r->mods);
}

void sh_attach_tags_map_end(void)
{
    if (g_registry.current) sh_md6_registry_publish(&g_registry, NULL);
}

void sh_attach_tags_command(sh_ce_line_fn line, void *context)
{
    sh_md6_registry *reg = (sh_md6_registry *)g_registry.current, *preview = NULL;
    int oom = 0;
    const char *state;
    char *text;
    if (!line) return;
    if (!reg) reg = preview = sh_md6_registry_build(SH_MD6_ATTACHMENT_TAGS, &oom);
    if (!reg || !reg->n) {
        line(context, oom ? "MODS: attachment-tags: out of memory" :
             "MODS: attachment-tags: the open map's mods give no tag info for their attachments (no mod.json "
             "\"attachment_tags\" in the builds it serves)");
        if (preview) { sh_md6_registry_free(preview); free(preview); }
        return;
    }
    state = sh_feature_is_off(SH_FEATURE_ATTACHMENT_TAGS) ? "Attachment tags is unavailable in this session"
          : g_committed ? "the tag-info lookup is detoured"
          : "the tag-info lookup is detoured at the first Play that registers a prop";
    text = sh_textf("MODS: attachment-tags: %d prop(s) of %d mod(s) %s; %s", reg->n, reg->mods,
                    preview ? "in the open map's mods, registered at its next Play" : "registered for this Play", state);
    if (text) line(context, text);
    free(text);
    sh_md6_registry_print(reg, SH_MD6_ATTACHMENT_TAGS, line, context);
    if (preview) { sh_md6_registry_free(preview); free(preview); }
}
