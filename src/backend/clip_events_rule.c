/* clip_events_rule.c -- the pure half of clip events (clip_events.h): names, the md6Def reader, the build's
 * registries rows, the per-Play registry and the chain rule. No engine, no global state. */
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "clip_events.h"
#include "mp_json.h"
#include "md6_tokens.h"
#include "../common/text.h"

static int ce_prefix(const char *s, const char *prefix, size_t n)
{
    size_t i;
    if (!s) return 0;
    for (i = 0; i < n; i++) if (!s[i] || sh_path_fold(s[i]) != sh_path_fold(prefix[i])) return 0;
    return 1;
}

static int ce_name_cmp(const char *a, const char *b)
{
    for (; *a && *b; ++a, ++b) {
        char x = sh_path_fold(*a), y = sh_path_fold(*b);
        if (x != y) return (unsigned char)x < (unsigned char)y ? -1 : 1;
    }
    return *a ? 1 : *b ? -1 : 0;
}

int sh_ce_under_mods(const char *clip)
{
    return ce_prefix(clip, SH_CE_CLIP_ROOT, SH_CE_CLIP_ROOT_LEN);
}

/* Tests that `name` is <root><salt>/<something>. */
static int ce_under(const char *name, const char *root, size_t root_len, const char *salt)
{
    size_t n = salt ? strlen(salt) : 0;
    if (!n || !ce_prefix(name, root, root_len)) return 0;
    if (!ce_prefix(name + root_len, salt, n)) return 0;
    name += root_len + n;
    return (*name == '/' || *name == '\\') && name[1] && name[1] != '/' && name[1] != '\\';
}

int sh_ce_clip_of_mod(const char *clip, const char *salt)
{
    return ce_under(clip, SH_CE_CLIP_ROOT, SH_CE_CLIP_ROOT_LEN, salt);
}

int sh_ce_def_of_mod(const char *md6def, const char *salt)
{
    return ce_under(md6def, SH_CE_DECL_ROOT, SH_CE_DECL_ROOT_LEN, salt);
}

int sh_ce_decl_under_mods(const char *name)
{
    return ce_prefix(name, SH_CE_DECL_ROOT, SH_CE_DECL_ROOT_LEN);
}

void sh_ce_md6_free(sh_ce_md6 *m)
{
    int i;
    if (!m) return;
    free(m->base);
    for (i = 0; i < m->nclips; i++) free(m->clips[i]);
    free(m->clips);
    memset(m, 0, sizeof *m);
}

int sh_ce_md6_read(const char *text, size_t len, sh_ce_md6 *out)
{
    size_t pos = 0;
    int depth = 0, in_init = 0, in_events = 0, cap = 0;
    md6_token prev = { MD6_END, NULL, 0 }, prev2 = { MD6_END, NULL, 0 };
    /* The anim block being read: its name, and a flag for an event row seen. */
    const char *clip = NULL;
    size_t clip_n = 0;
    int clip_depth = -1, clip_rows = 0;
    memset(out, 0, sizeof *out);
    if (!text) return 1;
    for (;;) {
        md6_token t = md6_next(text, len, &pos);
        if (t.k == MD6_END) break;
        if (t.k == MD6_LB) {
            depth++;
            /* `init {` and `events {` directly inside the decl's own braces. */
            if (depth == 2 && md6_word(&prev, "init")) in_init = 1;
            if (depth == 2 && md6_word(&prev, "events")) in_events = 1;
            if (depth == 3 && in_events && prev.k == MD6_STR && md6_word(&prev2, "anim")) {
                clip = prev.p; clip_n = prev.n; clip_depth = 3; clip_rows = 0;
            }
        } else if (t.k == MD6_RB) {
            if (depth == clip_depth) {
                if (clip_rows && clip_n) {
                    if (out->nclips == cap) {
                        int c = cap ? cap * 2 : 8;
                        char **g = (char **)realloc(out->clips, sizeof(char *) * (size_t)c);
                        if (!g) { sh_ce_md6_free(out); return 0; }
                        out->clips = g;
                        cap = c;
                    }
                    if (!(out->clips[out->nclips] = sh_textn(clip, clip_n))) { sh_ce_md6_free(out); return 0; }
                    out->nclips++;
                } else out->empty++;
                clip_depth = -1;
                clip = NULL;
            }
            if (depth == 2) in_init = in_events = 0;
            if (depth > 0) depth--;
        } else if (t.k == MD6_STR && in_init && depth == 2 && md6_word(&prev, "inherit") && !out->base) {
            if (t.n && !(out->base = sh_textn(t.p, t.n))) { sh_ce_md6_free(out); return 0; }
        } else if (t.k == MD6_WORD && clip_depth == 3 && depth == 3 && md6_word(&t, "event")) {
            clip_rows++;
        }
        prev2 = prev;
        prev = t;
    }
    return 1;
}

static void ce_say(sh_ce_line_fn fn, void *ctx, int *oom, const char *fmt, ...)
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

typedef struct ce_given { const char *clip; int entry; } ce_given;

char *sh_ce_build_registry(const char *salt, const char *const *names, int n, sh_ce_text_fn text, void *text_ctx,
                           sh_ce_line_fn warn, void *warn_ctx, int *oom)
{
    sh_text_buf out = { 0 };
    ce_given *given = NULL;
    sh_ce_md6 *read = NULL;
    int ngiven = 0, cgiven = 0, i, k, rows = 0, dummy = 0;
    if (!oom) oom = &dummy;
    *oom = 0;
    if (!salt || !names || n <= 0) return NULL;
    read = (sh_ce_md6 *)calloc((size_t)n, sizeof *read);
    if (!read) { *oom = 1; return NULL; }
    sh_text_append(&out, "[", 1);
    for (i = 0; i < n; i++) {
        const char *name = names[i];
        char *body = NULL;
        size_t blen = 0;
        int found, dup = 0, first = 1, kept = 0;
        if (!name || !name[0]) continue;
        for (k = 0; k < i && !dup; k++) dup = names[k] && sh_path_eq(names[k], name);
        if (dup) {
            ce_say(warn, warn_ctx, oom, "mod.json animation_events lists \"%s\" twice; the second entry is ignored. List "
                   "each md6Def once.", name);
            continue;
        }
        found = sh_ce_def_of_mod(name, salt) && text ? text(text_ctx, name, &body, &blen) : 0;
        if (found <= 0) {
            ce_say(warn, warn_ctx, oom, found < 0
                   ? "mod.json animation_events: \"%s\" could not be read, so it was left out. Check that the file is not "
                     "open in another program and press Refresh."
                   : "mod.json animation_events: \"%s\" is not a model definition of this mod, so it was left out. List an "
                     "md6Def this mod carries: the path of its file under assets/generated/decls/md6def/, from "
                     "mods/<folder name>/ on, without .decl.",
                   name);
            continue;
        }
        if (!sh_ce_md6_read(body, blen, &read[i])) { free(body); *oom = 1; break; }
        free(body);
        if (!read[i].base) {
            ce_say(warn, warn_ctx, oom, "mod.json animation_events: \"%s\" does not say which game model it adds rows to, "
                   "so it was left out. Start it with init { inherit \"<that model>\" }.", name);
            continue;
        }
        if (!read[i].nclips) {
            ce_say(warn, warn_ctx, oom, "mod.json animation_events: \"%s\" adds no event rows, so it was left out. Give it "
                   "an events { anim \"md6/mods/<folder name>/<clip>.md6anim\" { event ... } } block.", name);
            continue;
        }
        for (k = 0; k < read[i].nclips; k++) {
            const char *clip = read[i].clips[k];
            int g, other = -1;
            if (!sh_ce_clip_of_mod(clip, salt)) {
                ce_say(warn, warn_ctx, oom, "mod.json animation_events: \"%s\" adds rows to \"%s\", which is not a clip of "
                       "this mod, so those rows are not used. A mod can add rows only to its own clips, the ones "
                       "under md6/mods/<folder name>/.", name, clip);
                continue;
            }
            for (g = 0; g < ngiven && other < 0; g++) if (sh_path_eq(given[g].clip, clip)) other = given[g].entry;
            if (other >= 0) {
                if (other != i)
                    ce_say(warn, warn_ctx, oom, "mod.json animation_events: \"%s\" gets rows from \"%s\" and from \"%s\"; only "
                           "\"%s\"'s are used. Keep its rows in one md6Def.", clip, names[other], name, names[other]);
                continue;
            }
            if (ngiven == cgiven) {
                int c = cgiven ? cgiven * 2 : 16;
                ce_given *gr = (ce_given *)realloc(given, sizeof *given * (size_t)c);
                if (!gr) { *oom = 1; break; }
                given = gr;
                cgiven = c;
            }
            given[ngiven].clip = clip;
            given[ngiven].entry = i;
            ngiven++;
            if (first) {
                sh_text_append_str(&out, rows ? ",{\"md6Def\":" : "{\"md6Def\":");
                md6_put_name(&out, name);
                sh_text_append_str(&out, ",\"base\":");
                md6_put_name(&out, read[i].base);
                sh_text_append_str(&out, ",\"clips\":[");
                first = 0;
            } else sh_text_append(&out, ",", 1);
            md6_put_name(&out, clip);
            kept++;
        }
        if (kept) { sh_text_append_str(&out, "]}"); rows++; }
        if (*oom) break;
    }
    sh_text_append(&out, "]", 1);
    for (i = 0; i < n; i++) sh_ce_md6_free(&read[i]);
    free(read);
    free(given);
    if (out.oom) *oom = 1;
    if (*oom || !rows) { free(out.p); return NULL; }
    return out.p;
}

/* Returns a JSON string's text when it has no escapes, NULL for anything else. */
static const char *ce_plain(const mpj *v)
{
    if (!v || v->type != MPJ_STR || !v->text || !v->text[0] || strchr(v->text, '\\')) return NULL;
    return v->text;
}

static void ce_row_free(sh_ce_row *w)
{
    free(w->clip);
    free(w->carrier);
    free(w->base);
    free(w->salt);
    free(w->label);
}

int sh_ce_registry_add(sh_ce_registry *r, const char *salt, const char *label, const char *registries,
                       sh_ce_line_fn line, void *line_ctx)
{
    mpj *root, *list;
    int i, k, added = 0, failed = 0;
    const char *who = label && label[0] ? label : salt;
    if (!r || !salt || !registries || !strstr(registries, "\"" SH_CE_REGISTRY_KEY "\"")) return 0;
    root = mpj_parse(registries, strlen(registries));
    if (!root) {
        ce_say(line, line_ctx, NULL, "MODS: clip-events: %s: its build's record could not be read, so its clips play "
               "without the events it adds. Press Refresh in the Mods tab to build it again.", who);
        return 0;
    }
    list = root->type == MPJ_OBJ ? mpj_get(root, SH_CE_REGISTRY_KEY) : NULL;
    for (i = 0; list && list->type == MPJ_ARR && i < list->n && !failed; i++) {
        const mpj *e = list->items[i];
        const char *carrier = e && e->type == MPJ_OBJ ? ce_plain(mpj_get(e, "md6Def")) : NULL;
        const char *base = e && e->type == MPJ_OBJ ? ce_plain(mpj_get(e, "base")) : NULL;
        const mpj *clips = e && e->type == MPJ_OBJ ? mpj_get(e, "clips") : NULL;
        if (!carrier || !base || !clips || clips->type != MPJ_ARR || !sh_ce_def_of_mod(carrier, salt)) {
            ce_say(line, line_ctx, NULL, "MODS: clip-events: %s: a animation_events row of its build is not one of its own "
                   "md6Defs, so it is not used. Press Refresh in the Mods tab to build the mod again.", who);
            continue;
        }
        for (k = 0; k < clips->n; k++) {
            const char *clip = ce_plain(clips->items[k]);
            sh_ce_row *w;
            int j, dup = 0;
            if (!clip || !sh_ce_clip_of_mod(clip, salt)) {
                ce_say(line, line_ctx, NULL, "MODS: clip-events: %s: \"%s\" adds rows to a clip that is not its own, "
                       "so they are not used.", who, carrier);
                continue;
            }
            for (j = 0; j < r->n && !dup; j++) dup = sh_path_eq(r->rows[j].clip, clip);
            if (dup) continue;
            if (r->n == r->cap) {
                int c = r->cap ? r->cap * 2 : 16;
                sh_ce_row *g = (sh_ce_row *)realloc(r->rows, sizeof *g * (size_t)c);
                if (!g) { failed = 1; break; }
                r->rows = g;
                r->cap = c;
            }
            w = &r->rows[r->n];
            memset(w, 0, sizeof *w);
            w->clip = sh_text(clip);
            w->carrier = sh_text(carrier);
            w->base = sh_text(base);
            w->salt = sh_text(salt);
            w->label = sh_text(who);
            if (!w->clip || !w->carrier || !w->base || !w->salt || !w->label) { ce_row_free(w); failed = 1; break; }
            r->n++;
            added++;
        }
    }
    mpj_free(root);
    if (added) r->mods++;
    return failed ? -1 : added;
}

static int ce_row_cmp(const void *a, const void *b)
{
    return ce_name_cmp(((const sh_ce_row *)a)->clip, ((const sh_ce_row *)b)->clip);
}

void sh_ce_registry_seal(sh_ce_registry *r)
{
    if (r && r->n > 1) qsort(r->rows, (size_t)r->n, sizeof *r->rows, ce_row_cmp);
}

sh_ce_row *sh_ce_registry_find(const sh_ce_registry *r, const char *clip)
{
    int lo = 0, hi;
    if (!r || !clip || !r->n) return NULL;
    hi = r->n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = ce_name_cmp(clip, r->rows[mid].clip);
        if (!c) return &r->rows[mid];
        if (c < 0) hi = mid - 1;
        else lo = mid + 1;
    }
    return NULL;
}

void sh_ce_registry_free(sh_ce_registry *r)
{
    int i;
    if (!r) return;
    for (i = 0; i < r->n; i++) ce_row_free(&r->rows[i]);
    free(r->rows);
    memset(r, 0, sizeof *r);
}

/* Returns 1 when `target` is `from` or one of its ancestors, -1 when a parent could not be read. */
static int ce_reaches(const void *from, const void *target, sh_ce_parent_fn parent, void *context)
{
    int hops;
    for (hops = 0; from && hops <= SH_CE_CHAIN_MAX; hops++) {
        const void *up = NULL;
        if (from == target) return 1;
        if (!parent(context, from, &up)) return -1;
        from = up;
    }
    return 0;
}

int sh_ce_in_chain(const void *carrier, const void *model, sh_ce_parent_fn parent, void *context)
{
    const void *base = NULL;
    int r;
    if (!carrier || !model || !parent) return 0;
    r = ce_reaches(carrier, model, parent, context);
    if (r) return r > 0;
    /* `model` inherits the carrier's base: a body below the one the carrier names. */
    if (!parent(context, carrier, &base) || !base) return 0;
    return ce_reaches(model, base, parent, context) > 0;
}
