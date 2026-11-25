#include "md6_registry_engine.h"
#include "clip_events.h"
#include "attach_tags.h"
#include "modserve.h"
#include "backend_log.h"
#include "../common/text.h"

#define MODEL_NAME   0x08u
#define MODEL_PARENT 0x98u

void sh_md6_log(const char *fmt, ...)
{
    char *line;
    va_list ap;
    va_start(ap, fmt);
    line = sh_vtextf(fmt, ap);
    va_end(ap);
    if (line) backend_log(line);
    free(line);
}

static void registry_line(void *context, const char *line)
{
    (void)context;
    backend_log(line);
}

int sh_md6_av_only(DWORD code)
{
    return code == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}

int sh_md6_parent(void *context, const void *model, const void **parent)
{
    (void)context;
    __try { *parent = *(const void *const *)((const uint8_t *)model + MODEL_PARENT); }
    __except (sh_md6_av_only(GetExceptionCode())) { *parent = NULL; return 0; }
    return 1;
}

const char *sh_md6_name_at(const void *object, size_t offset)
{
    const char *name = NULL;
    __try { name = *(const char *const *)((const uint8_t *)object + offset); }
    __except (sh_md6_av_only(GetExceptionCode())) { name = NULL; }
    return name;
}

/* Returns 1 when the object at `model` is still the md6Def `name`. */
static int md6_still(const void *model, const char *name)
{
    int same = 0;
    __try { same = sh_path_eq(*(const char *const *)((const uint8_t *)model + MODEL_NAME), name); }
    __except (sh_md6_av_only(GetExceptionCode())) { same = 0; }
    return same;
}

/* Returns the carrier of a row in this Play, found by name, or NULL. */
void *sh_md6_model(sh_md6_row *row, sh_md6_registry_kind kind)
{
    void *c = row->model;
    if (row->state == SH_MD6_ROW_MISSING) return NULL;
    if (c && md6_still(c, row->carrier)) return c;
    row->model = NULL;
    if (sh_modserve_engine_on_main() == 1) {
        c = sh_modserve_engine_decl("md6Def", row->carrier);
        if (!c) {
            if (InterlockedExchange(&row->state, SH_MD6_ROW_MISSING) != SH_MD6_ROW_MISSING)
                sh_md6_log(kind == SH_MD6_CLIP_EVENTS ?
                    "MODS: clip-events: %s: \"%s\" did not load, so \"%s\" plays without the events it adds. Open "
                       "the Mods tab: the mod's warnings, or the backend log line before this one, say what to fix." :
                    "MODS: attachment-tags: %s: \"%s\" did not load, so \"%s\" attaches without the tag info it "
                       "gives. Open the Mods tab: the mod's warnings, or the backend log line before this one, say "
                       "what to fix.", row->label, row->carrier, row->name);
            return NULL;
        }
    } else {
        void *o = NULL;
        unsigned state = 0;
        if (sh_modserve_engine_decl_held("md6Def", row->carrier, &o, &state, NULL) != 1 || (state & 3))
            return NULL;   /* not loaded yet: the main thread's next lookup loads it */
        c = o;
    }
    row->model = c;
    InterlockedExchange(&row->state, SH_MD6_ROW_READY);
    return c;
}

typedef struct registry_build { sh_md6_registry *r; sh_md6_registry_kind kind; int oom; } registry_build;

static void add_bundle(void *context, const char *id, const char *hash, const char *registries)
{
    registry_build *b = (registry_build *)context;
    sh_modserve_scope_info info;
    char *label;
    const char *key = b->kind == SH_MD6_CLIP_EVENTS ? "\"" SH_CE_REGISTRY_KEY "\"" : "\"" SH_AT_REGISTRY_KEY "\"";
    if (!registries || !strstr(registries, key) || !hash) return;
    /* Only the builds the open map serves. */
    if (!sh_modserve_bundle_scope(hash, &info) || !info.names_live) return;
    label = sh_modserve_bundle_label(hash);
    if (sh_md6_registry_add(b->r, b->kind, id, label ? label : id, registries, registry_line, NULL) < 0) b->oom = 1;
    free(label);
}

sh_md6_registry *sh_md6_registry_build(sh_md6_registry_kind kind, int *oom)
{
    registry_build b;
    b.kind = kind;
    b.oom = 0;
    b.r = (sh_md6_registry *)calloc(1, sizeof *b.r);
    if (!b.r) { *oom = 1; return NULL; }
    (void)sh_modserve_each_bundle_registries(add_bundle, &b);
    sh_md6_registry_seal(b.r);
    *oom = b.oom;
    return b.r;
}

void sh_md6_registry_publish(sh_md6_runtime *runtime, sh_md6_registry *r)
{
    sh_md6_registry *old;
    InterlockedExchange(&runtime->any, 0);
    old = (sh_md6_registry *)InterlockedExchangePointer((void *volatile *)&runtime->current, r);
    /* A lookup still reading the one before last has returned: two gates lie between. */
    if (runtime->retired) { sh_md6_registry_free(runtime->retired); free(runtime->retired); }
    runtime->retired = old;
    if (r && r->n) InterlockedExchange(&runtime->any, 1);
}

void sh_md6_registry_print(const sh_md6_registry *reg, sh_md6_registry_kind kind, sh_ce_line_fn line, void *context)
{
    int i, k;
    char *text;
    for (i = 0; i < reg->n; i++) {
        const sh_md6_row *w = &reg->rows[i];
        void *obj = NULL;
        unsigned st = 0, level = 0;
        int held;
        for (k = 0; k < i && !sh_path_eq(reg->rows[k].carrier, w->carrier); k++) {}
        if (k < i) continue;   /* Each carrier once, with its clips. */
        held = sh_modserve_engine_decl_held("md6Def", w->carrier, &obj, &st, &level);
        text = sh_textf("  %s: \"%s\" over \"%s\": %s", w->label, w->carrier, w->base,
                        w->state == SH_MD6_ROW_MISSING ? "did not load in this Play"
                        : held == 1 && !(st & 3) ? (level == 4 ? "loaded (kept for the session)" : "loaded")
                        : held == 1 ? "loaded as a default (its file was refused)"
                        : held == 0 ? (kind == SH_MD6_CLIP_EVENTS ? "not loaded (it loads at the first lookup of one of its clips in Play)"
                                      : "not loaded (it loads at the first lookup of one of its props in Play)")
                        : "unknown (the engine's lists could not be read)");
        if (text) line(context, text);
        free(text);
        for (k = i; k < reg->n; k++) {
            const sh_md6_row *c = &reg->rows[k];
            if (!sh_path_eq(c->carrier, w->carrier)) continue;
            text = c->answered ? sh_textf("    \"%s\": answered %ld lookup(s)", c->name, (long)c->answered)
                               : sh_textf("    \"%s\"", c->name);
            if (text) line(context, text);
            free(text);
        }
    }
}
