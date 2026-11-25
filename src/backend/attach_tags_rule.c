/* attach_tags_rule.c -- the pure half of attachment tags (attach_tags.h): names, the md6Def props reader, the
 * build's registries rows and the per-Play registry. */
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "attach_tags.h"
#include "mp_json.h"
#include "md6_tokens.h"
#include "../common/text.h"

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
            sh_md6_registry_note(warn, warn_ctx, oom, "mod.json attachment_tags lists \"%s\" twice; the second entry is ignored. "
                   "List each md6Def once.", name);
            continue;
        }
        found = sh_ce_def_of_mod(name, salt) && text ? text(text_ctx, name, &body, &blen) : 0;
        if (found <= 0) {
            sh_md6_registry_note(warn, warn_ctx, oom, found < 0
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
            sh_md6_registry_note(warn, warn_ctx, oom, "mod.json attachment_tags: \"%s\" does not say which game model it gives tag "
                   "info for, so it was left out. Start it with init { inherit \"<that model>\" }.", name);
            continue;
        }
        if (!read[i].nprops) {
            sh_md6_registry_note(warn, warn_ctx, oom, "mod.json attachment_tags: \"%s\" gives no tag info, so it was left out. Give "
                   "it a props { prop \"mods/<folder name>/<attached md6Def>.md6\" { tag \"<tag name>\" { trans ( x y "
                   "z ) rot ( x y z w ) parent \"<joint>\" } } } block.", name);
            continue;
        }
        for (k = 0; k < read[i].nprops; k++) {
            const char *prop = read[i].props[k];
            int g, other = -1;
            if (!sh_ce_def_of_mod(prop, salt)) {
                sh_md6_registry_note(warn, warn_ctx, oom, "mod.json attachment_tags: \"%s\" gives tag info for \"%s\", which is not "
                       "an md6Def of this mod, so it is not used. A mod can give tag info only for its own md6Defs, "
                       "the ones under mods/<folder name>/.", name, prop);
                continue;
            }
            for (g = 0; g < ngiven && other < 0; g++) if (sh_path_eq(given[g].prop, prop)) other = given[g].entry;
            if (other >= 0) {
                if (other != i)
                    sh_md6_registry_note(warn, warn_ctx, oom, "mod.json attachment_tags: \"%s\" gets tag info from \"%s\" and from "
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
