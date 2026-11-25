/* attach_tags_rule.c -- the pure half of attachment tags (attach_tags.h): names, the md6Def props reader, the
 * build's registries rows and the per-Play registry. */
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "attach_tags.h"
#include "mp_json.h"
#include "md6_tokens.h"
#include "../common/text.h"

static int at_name_cmp(const char *a, const char *b)
{
    for (; *a && *b; ++a, ++b) {
        char x = sh_path_fold(*a), y = sh_path_fold(*b);
        if (x != y) return (unsigned char)x < (unsigned char)y ? -1 : 1;
    }
    return *a ? 1 : *b ? -1 : 0;
}

int sh_at_under_mods(const char *prop)
{
    return sh_ce_decl_under_mods(prop);
}

int sh_at_prop_of_mod(const char *prop, const char *salt)
{
    return sh_ce_def_of_mod(prop, salt);
}

void sh_at_md6_free(sh_at_md6 *m)
{
    int i;
    if (!m) return;
    free(m->base);
    for (i = 0; i < m->nprops; i++) free(m->props[i]);
    free(m->props);
    memset(m, 0, sizeof *m);
}

int sh_at_md6_read(const char *text, size_t len, sh_at_md6 *out)
{
    size_t pos = 0;
    int depth = 0, in_init = 0, in_props = 0, cap = 0;
    md6_token prev = { MD6_END, NULL, 0 }, prev2 = { MD6_END, NULL, 0 };
    /* The prop block being read: its name, and a flag for a tag seen. */
    const char *prop = NULL;
    size_t prop_n = 0;
    int prop_depth = -1, prop_tags = 0;
    memset(out, 0, sizeof *out);
    if (!text) return 1;
    for (;;) {
        md6_token t = md6_next(text, len, &pos);
        if (t.k == MD6_END) break;
        if (t.k == MD6_LB) {
            depth++;
            /* `init {` and `props {` directly inside the decl's own braces. */
            if (depth == 2 && md6_word(&prev, "init")) in_init = 1;
            if (depth == 2 && md6_word(&prev, "props")) in_props = 1;
            if (depth == 3 && in_props && prev.k == MD6_STR && md6_word(&prev2, "prop")) {
                prop = prev.p; prop_n = prev.n; prop_depth = 3; prop_tags = 0;
            }
        } else if (t.k == MD6_RB) {
            if (depth == prop_depth) {
                if (prop_tags && prop_n) {
                    if (out->nprops == cap) {
                        int c = cap ? cap * 2 : 8;
                        char **g = (char **)realloc(out->props, sizeof(char *) * (size_t)c);
                        if (!g) { sh_at_md6_free(out); return 0; }
                        out->props = g;
                        cap = c;
                    }
                    if (!(out->props[out->nprops] = sh_textn(prop, prop_n))) { sh_at_md6_free(out); return 0; }
                    out->nprops++;
                } else out->empty++;
                prop_depth = -1;
                prop = NULL;
            }
            if (depth == 2) in_init = in_props = 0;
            if (depth > 0) depth--;
        } else if (t.k == MD6_STR && in_init && depth == 2 && md6_word(&prev, "inherit") && !out->base) {
            if (t.n && !(out->base = sh_textn(t.p, t.n))) { sh_at_md6_free(out); return 0; }
        } else if (t.k == MD6_WORD && prop_depth == 3 && depth == 3 && md6_word(&t, "tag")) {
            prop_tags++;
        }
        prev2 = prev;
        prev = t;
    }
    return 1;
}

static void at_say(sh_ce_line_fn fn, void *ctx, int *oom, const char *fmt, ...)
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

typedef struct at_given { const char *prop; int entry; } at_given;

char *sh_at_build_registry(const char *salt, const char *const *names, int n, sh_ce_text_fn text, void *text_ctx,
                           sh_ce_line_fn warn, void *warn_ctx, int *oom)
{
    sh_text_buf out = { 0 };
    at_given *given = NULL;
    sh_at_md6 *read = NULL;
    int ngiven = 0, cgiven = 0, i, k, rows = 0, dummy = 0;
    if (!oom) oom = &dummy;
    *oom = 0;
    if (!salt || !names || n <= 0) return NULL;
    read = (sh_at_md6 *)calloc((size_t)n, sizeof *read);
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
            at_say(warn, warn_ctx, oom, "mod.json attachment_tags lists \"%s\" twice; the second entry is ignored. "
                   "List each md6Def once.", name);
            continue;
        }
        found = sh_ce_def_of_mod(name, salt) && text ? text(text_ctx, name, &body, &blen) : 0;
        if (found <= 0) {
            at_say(warn, warn_ctx, oom, found < 0
                   ? "mod.json attachment_tags: \"%s\" could not be read, so it was left out. Check that the file is "
                     "not open in another program and press Refresh."
                   : "mod.json attachment_tags: \"%s\" is not a model definition of this mod, so it was left out. List "
                     "an md6Def this mod carries: the path of its file under assets/generated/decls/md6def/, from "
                     "mods/<folder name>/ on, without .decl.",
                   name);
            continue;
        }
        if (!sh_at_md6_read(body, blen, &read[i])) { free(body); *oom = 1; break; }
        free(body);
        if (!read[i].base) {
            at_say(warn, warn_ctx, oom, "mod.json attachment_tags: \"%s\" does not say which game model it gives tag "
                   "info for, so it was left out. Start it with init { inherit \"<that model>\" }.", name);
            continue;
        }
        if (!read[i].nprops) {
            at_say(warn, warn_ctx, oom, "mod.json attachment_tags: \"%s\" gives no tag info, so it was left out. Give "
                   "it a props { prop \"mods/<folder name>/<attached md6Def>.md6\" { tag \"<tag name>\" { trans ( x y "
                   "z ) rot ( x y z w ) parent \"<joint>\" } } } block.", name);
            continue;
        }
        for (k = 0; k < read[i].nprops; k++) {
            const char *prop = read[i].props[k];
            int g, other = -1;
            if (!sh_at_prop_of_mod(prop, salt)) {
                at_say(warn, warn_ctx, oom, "mod.json attachment_tags: \"%s\" gives tag info for \"%s\", which is not "
                       "an md6Def of this mod, so it is not used. A mod can give tag info only for its own md6Defs, "
                       "the ones under mods/<folder name>/.", name, prop);
                continue;
            }
            for (g = 0; g < ngiven && other < 0; g++) if (sh_path_eq(given[g].prop, prop)) other = given[g].entry;
            if (other >= 0) {
                if (other != i)
                    at_say(warn, warn_ctx, oom, "mod.json attachment_tags: \"%s\" gets tag info from \"%s\" and from "
                           "\"%s\"; only \"%s\"'s is used. Keep its tag info in one md6Def.", prop, names[other], name,
                           names[other]);
                continue;
            }
            if (ngiven == cgiven) {
                int c = cgiven ? cgiven * 2 : 16;
                at_given *gr = (at_given *)realloc(given, sizeof *given * (size_t)c);
                if (!gr) { *oom = 1; break; }
                given = gr;
                cgiven = c;
            }
            given[ngiven].prop = prop;
            given[ngiven].entry = i;
            ngiven++;
            if (first) {
                sh_text_append_str(&out, rows ? ",{\"md6Def\":" : "{\"md6Def\":");
                md6_put_name(&out, name);
                sh_text_append_str(&out, ",\"base\":");
                md6_put_name(&out, read[i].base);
                sh_text_append_str(&out, ",\"props\":[");
                first = 0;
            } else sh_text_append(&out, ",", 1);
            md6_put_name(&out, prop);
            kept++;
        }
        if (kept) { sh_text_append_str(&out, "]}"); rows++; }
        if (*oom) break;
    }
    sh_text_append(&out, "]", 1);
    for (i = 0; i < n; i++) sh_at_md6_free(&read[i]);
    free(read);
    free(given);
    if (out.oom) *oom = 1;
    if (*oom || !rows) { free(out.p); return NULL; }
    return out.p;
}

/* Returns a JSON string's text when it has no escapes, NULL for anything else. */
static const char *at_plain(const mpj *v)
{
    if (!v || v->type != MPJ_STR || !v->text || !v->text[0] || strchr(v->text, '\\')) return NULL;
    return v->text;
}

static void at_row_free(sh_at_row *w)
{
    free(w->prop);
    free(w->carrier);
    free(w->base);
    free(w->salt);
    free(w->label);
}

int sh_at_registry_add(sh_at_registry *r, const char *salt, const char *label, const char *registries,
                       sh_ce_line_fn line, void *line_ctx)
{
    mpj *root, *list;
    int i, k, added = 0, failed = 0;
    const char *who = label && label[0] ? label : salt;
    if (!r || !salt || !registries || !strstr(registries, "\"" SH_AT_REGISTRY_KEY "\"")) return 0;
    root = mpj_parse(registries, strlen(registries));
    if (!root) {
        at_say(line, line_ctx, NULL, "MODS: attachment-tags: %s: its build's record could not be read, so the tag info "
               "it gives is not used. Press Refresh in the Mods tab to build it again.", who);
        return 0;
    }
    list = root->type == MPJ_OBJ ? mpj_get(root, SH_AT_REGISTRY_KEY) : NULL;
    for (i = 0; list && list->type == MPJ_ARR && i < list->n && !failed; i++) {
        const mpj *e = list->items[i];
        const char *carrier = e && e->type == MPJ_OBJ ? at_plain(mpj_get(e, "md6Def")) : NULL;
        const char *base = e && e->type == MPJ_OBJ ? at_plain(mpj_get(e, "base")) : NULL;
        const mpj *props = e && e->type == MPJ_OBJ ? mpj_get(e, "props") : NULL;
        if (!carrier || !base || !props || props->type != MPJ_ARR || !sh_ce_def_of_mod(carrier, salt)) {
            at_say(line, line_ctx, NULL, "MODS: attachment-tags: %s: an attachment_tags row of its build is not one of "
                   "its own md6Defs, so it is not used. Press Refresh in the Mods tab to build the mod again.", who);
            continue;
        }
        for (k = 0; k < props->n; k++) {
            const char *prop = at_plain(props->items[k]);
            sh_at_row *w;
            int j, dup = 0;
            if (!prop || !sh_at_prop_of_mod(prop, salt)) {
                at_say(line, line_ctx, NULL, "MODS: attachment-tags: %s: \"%s\" gives tag info for an md6Def that is "
                       "not its own, so it is not used.", who, carrier);
                continue;
            }
            for (j = 0; j < r->n && !dup; j++) dup = sh_path_eq(r->rows[j].prop, prop);
            if (dup) continue;
            if (r->n == r->cap) {
                int c = r->cap ? r->cap * 2 : 16;
                sh_at_row *g = (sh_at_row *)realloc(r->rows, sizeof *g * (size_t)c);
                if (!g) { failed = 1; break; }
                r->rows = g;
                r->cap = c;
            }
            w = &r->rows[r->n];
            memset(w, 0, sizeof *w);
            w->prop = sh_text(prop);
            w->carrier = sh_text(carrier);
            w->base = sh_text(base);
            w->salt = sh_text(salt);
            w->label = sh_text(who);
            if (!w->prop || !w->carrier || !w->base || !w->salt || !w->label) { at_row_free(w); failed = 1; break; }
            r->n++;
            added++;
        }
    }
    mpj_free(root);
    if (added) r->mods++;
    return failed ? -1 : added;
}

static int at_row_cmp(const void *a, const void *b)
{
    return at_name_cmp(((const sh_at_row *)a)->prop, ((const sh_at_row *)b)->prop);
}

void sh_at_registry_seal(sh_at_registry *r)
{
    if (r && r->n > 1) qsort(r->rows, (size_t)r->n, sizeof *r->rows, at_row_cmp);
}

sh_at_row *sh_at_registry_find(const sh_at_registry *r, const char *prop)
{
    int lo = 0, hi;
    if (!r || !prop || !r->n) return NULL;
    hi = r->n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = at_name_cmp(prop, r->rows[mid].prop);
        if (!c) return &r->rows[mid];
        if (c < 0) hi = mid - 1;
        else lo = mid + 1;
    }
    return NULL;
}

void sh_at_registry_free(sh_at_registry *r)
{
    int i;
    if (!r) return;
    for (i = 0; i < r->n; i++) at_row_free(&r->rows[i]);
    free(r->rows);
    memset(r, 0, sizeof *r);
}
