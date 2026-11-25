#include "md6_registry.h"
#include "clip_events.h"
#include "attach_tags.h"
#include "mp_json.h"
#include "../common/text.h"

static int name_cmp(const char *a, const char *b)
{
    for (; *a && *b; ++a, ++b) {
        char x = sh_path_fold(*a), y = sh_path_fold(*b);
        if (x != y) return (unsigned char)x < (unsigned char)y ? -1 : 1;
    }
    return *a ? 1 : *b ? -1 : 0;
}

void sh_md6_registry_note(sh_ce_line_fn fn, void *ctx, int *oom, const char *fmt, ...)
{
    char *line;
    va_list ap;
    va_start(ap, fmt);
    line = sh_vtextf(fmt, ap);
    va_end(ap);
    if (!line) { if (oom) *oom = 1; return; }
    if (fn) fn(ctx, line);
    free(line);
}

/* Returns a JSON string's text when it has no escapes, NULL for anything else. */
static const char *plain(const mpj *v)
{
    if (!v || v->type != MPJ_STR || !v->text || !v->text[0] || strchr(v->text, '\\')) return NULL;
    return v->text;
}

static void row_free(sh_md6_row *w)
{
    free(w->name);
    free(w->carrier);
    free(w->base);
    free(w->salt);
    free(w->label);
}

int sh_md6_registry_add(sh_md6_registry *r, sh_md6_registry_kind kind, const char *salt, const char *label, const char *registries,
                       sh_ce_line_fn line, void *line_ctx)
{
    int clip_events = kind == SH_MD6_CLIP_EVENTS;
    const char *key = clip_events ? SH_CE_REGISTRY_KEY : SH_AT_REGISTRY_KEY;
    const char *needle = clip_events ? "\"" SH_CE_REGISTRY_KEY "\"" : "\"" SH_AT_REGISTRY_KEY "\"";
    mpj *root, *list;
    int i, k, added = 0, failed = 0;
    const char *who = label && label[0] ? label : salt;
    if (!r || !salt || !registries || !strstr(registries, needle)) return 0;
    root = mpj_parse(registries, strlen(registries));
    if (!root) {
        sh_md6_registry_note(line, line_ctx, NULL, clip_events ?
                "MODS: clip-events: %s: its build's record could not be read, so its clips play "
                "without the events it adds. Press Refresh in the Mods tab to build it again." :
                "MODS: attachment-tags: %s: its build's record could not be read, so the tag info "
                "it gives is not used. Press Refresh in the Mods tab to build it again.", who);
        return 0;
    }
    list = root->type == MPJ_OBJ ? mpj_get(root, key) : NULL;
    for (i = 0; list && list->type == MPJ_ARR && i < list->n && !failed; i++) {
        const mpj *e = list->items[i];
        const char *carrier = e && e->type == MPJ_OBJ ? plain(mpj_get(e, "md6Def")) : NULL;
        const char *base = e && e->type == MPJ_OBJ ? plain(mpj_get(e, "base")) : NULL;
        const mpj *entries = e && e->type == MPJ_OBJ ? mpj_get(e, clip_events ? "clips" : "props") : NULL;
        if (!carrier || !base || !entries || entries->type != MPJ_ARR || !sh_ce_def_of_mod(carrier, salt)) {
            sh_md6_registry_note(line, line_ctx, NULL, clip_events ?
                "MODS: clip-events: %s: a animation_events row of its build is not one of its own "
                    "md6Defs, so it is not used. Press Refresh in the Mods tab to build the mod again." :
                "MODS: attachment-tags: %s: an attachment_tags row of its build is not one of "
                    "its own md6Defs, so it is not used. Press Refresh in the Mods tab to build the mod again.", who);
            continue;
        }
        for (k = 0; k < entries->n; k++) {
            const char *name = plain(entries->items[k]);
            sh_md6_row *w;
            int j, dup = 0;
            if (!name || !(clip_events ? sh_ce_clip_of_mod(name, salt) : sh_ce_def_of_mod(name, salt))) {
                sh_md6_registry_note(line, line_ctx, NULL, clip_events ?
                "MODS: clip-events: %s: \"%s\" adds rows to a clip that is not its own, "
                        "so they are not used." :
                "MODS: attachment-tags: %s: \"%s\" gives tag info for an md6Def that is "
                        "not its own, so it is not used.", who, carrier);
                continue;
            }
            for (j = 0; j < r->n && !dup; j++) dup = sh_path_eq(r->rows[j].name, name);
            if (dup) continue;
            if (r->n == r->cap) {
                int c = r->cap ? r->cap * 2 : 16;
                sh_md6_row *g = (sh_md6_row *)realloc(r->rows, sizeof *g * (size_t)c);
                if (!g) { failed = 1; break; }
                r->rows = g;
                r->cap = c;
            }
            w = &r->rows[r->n];
            memset(w, 0, sizeof *w);
            w->name = sh_text(name);
            w->carrier = sh_text(carrier);
            w->base = sh_text(base);
            w->salt = sh_text(salt);
            w->label = sh_text(who);
            if (!w->name || !w->carrier || !w->base || !w->salt || !w->label) { row_free(w); failed = 1; break; }
            r->n++;
            added++;
        }
    }
    mpj_free(root);
    if (added) r->mods++;
    return failed ? -1 : added;
}

static int row_cmp(const void *a, const void *b)
{
    return name_cmp(((const sh_md6_row *)a)->name, ((const sh_md6_row *)b)->name);
}

void sh_md6_registry_seal(sh_md6_registry *r)
{
    if (r && r->n > 1) qsort(r->rows, (size_t)r->n, sizeof *r->rows, row_cmp);
}

sh_md6_row *sh_md6_registry_find(const sh_md6_registry *r, const char *name)
{
    int lo = 0, hi;
    if (!r || !name || !r->n) return NULL;
    hi = r->n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = name_cmp(name, r->rows[mid].name);
        if (!c) return &r->rows[mid];
        if (c < 0) hi = mid - 1;
        else lo = mid + 1;
    }
    return NULL;
}

void sh_md6_registry_free(sh_md6_registry *r)
{
    int i;
    if (!r) return;
    for (i = 0; i < r->n; i++) row_free(&r->rows[i]);
    free(r->rows);
    memset(r, 0, sizeof *r);
}
