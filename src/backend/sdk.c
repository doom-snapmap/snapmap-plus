/* sdk.c -- the Graphics SDK's compilers, one entry point (sdk.h). */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "sdk.h"
#include "modeffect.h"
#include "modenv.h"
#include "modlook.h"
#include "modprog.h"
#include "modwound.h"
#include "sdkvalues.h"

/* The offset of the type folder after "generated/decls/" in `path`, or -1. */
static long type_at(const char *path)
{
    const char *head = "generated/decls/";
    size_t hn = strlen(head), n = path ? strlen(path) : 0, i;
    for (i = 0; i + hn <= n; i++)
        if (!_strnicmp(path + i, head, hn) && (i == 0 || path[i - 1] == '/' || path[i - 1] == '\\')) return (long)(i + hn);
    return -1;
}

static const char *sh_sdk_decl_type(const char *path)
{
    long at = type_at(path);
    const char *t;
    if (at < 0) return NULL;
    t = path + at;
    if (!_strnicmp(t, "material/", 9) || !_strnicmp(t, "material2/", 10)) return "material";
    if (!_strnicmp(t, "fx/", 3)) return "fx";
    if (!_strnicmp(t, "env/", 4)) return "env";
    if (!_strnicmp(t, "gorewounds/", 11)) return "gorewounds";
    return NULL;
}

char *sh_sdk_decl_name(const char *path)
{
    long at = type_at(path);
    const char *s, *e;
    char *out;
    size_t n, i;
    if (at < 0) return NULL;
    s = strchr(path + at, '/');
    if (!s) return NULL;
    s++;
    n = strlen(s);
    if (n > 5 && !_stricmp(s + n - 5, ".decl")) n -= 5;
    e = s + n;
    if (!(out = (char *)malloc(n + 1))) return NULL;
    for (i = 0; s + i < e; i++) out[i] = s[i] == '\\' ? '/' : (char)tolower((unsigned char)s[i]);
    out[n] = 0;
    return out;
}

/* Joins two note texts (either may be NULL); frees both. */
static char *join(char *a, char *b)
{
    size_t an, bn;
    char *r;
    if (!a) return b;
    if (!b) return a;
    an = strlen(a);
    bn = strlen(b);
    r = (char *)malloc(an + bn + 1);
    if (r) { memcpy(r, a, an); memcpy(r + an, b, bn + 1); }
    free(a);
    free(b);
    return r;
}

int sh_sdk_compile(const char *path, const char *text, size_t len, char **out, size_t *out_len, char *why,
                   size_t why_len, char **notes, char **needs)
{
    const char *type = sh_sdk_decl_type(path);
    *out = NULL;
    if (out_len) *out_len = 0;
    if (notes) *notes = NULL;
    if (needs) *needs = NULL;
    if (why && why_len) why[0] = 0;
    if (!type) return SH_LOOK_SAME;
    if (!strcmp(type, "fx")) return sh_effect_compile(path, text, len, out, out_len, why, why_len, notes, needs);
    if (!strcmp(type, "env")) return sh_env_compile(path, text, len, out, out_len, why, why_len, needs, notes);
    if (!strcmp(type, "gorewounds")) return sh_wound_compile(path, text, len, out, out_len, why, why_len);
    {
        /* a material: its values that change over time, then its type */
        char *v = NULL, *l = NULL, *ln = NULL;
        size_t vn = 0, lnn = 0;
        int rv = sdk_values_material_text(path, text, len, &v, &vn, why, why_len), rl;
        if (rv == 2) return SH_LOOK_MISTAKE;
        if (rv < 0) return SH_LOOK_OOM;
        rl = sh_look_compile_notes(path, rv == 1 ? v : text, rv == 1 ? vn : len, &l, &lnn, why, why_len,
                                   notes ? &ln : NULL);
        if (rl == SH_LOOK_MISTAKE || rl == SH_LOOK_OOM) { free(v); free(l); free(ln); return rl; }
        if (notes) *notes = join(*notes, ln);
        if (rl == SH_LOOK_CHANGED) { free(v); *out = l; if (out_len) *out_len = lnn; return SH_LOOK_CHANGED; }
        if (rv == 1) { *out = v; if (out_len) *out_len = vn; return SH_LOOK_CHANGED; }
        return SH_LOOK_SAME;
    }
}

int sh_sdk_serve_compile(const char *path, const char *text, size_t len, char **out, size_t *out_len, char *why,
                         size_t why_len)
{
    const char *kind = sh_sdk_decl_type(path);
    char *earlier = NULL;
    size_t n = 0;
    if (text && kind && !strcmp(kind, "material") && sh_modprog_pinned_material(text, len, &earlier, &n) < 0)
        return SH_LOOK_OOM;
    int r = sh_sdk_compile(path, earlier ? earlier : text, earlier ? n : len, out, out_len, why, why_len, NULL, NULL);
    if (earlier && r == SH_LOOK_SAME) {
        *out = earlier;
        if (out_len) *out_len = n;
        return SH_LOOK_CHANGED;
    }
    free(earlier);
    return r;
}
