/* sdkvalues.c -- values that change over time (sdkvalues.h). */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdkvalues.h"
#include "modprog.h"

#ifdef SH_SDK_FIXTURE
static sdk_register_fn g_register;
void sdk_set_register(sdk_register_fn fn) { g_register = fn; }
#endif

const char *sdk_register(const char *type, const char *text)
{
#ifdef SH_SDK_FIXTURE
    if (g_register) return g_register(type, text);
#endif
    return sh_modprog_register_decl(type, text);
}

/* The render parms whose decl is flagged srgba. */
static const char *const SRGB_PARMS[] = {
    "bloodcolor", "bloodcolorlerp", "bloomcolor", "cloudamblight", "cloudcovercolorparms", "color", "color2", "color3",
    "color4", "custombloomalbedo", "customdirtalbedo", "customdirtspecularsmoothness", "custompattern2albedo0",
    "custompattern2albedo1", "custompattern2specularsmoothness0", "custompattern2specularsmoothness1",
    "custompatternalbedo0", "custompatternalbedo1", "custompatternspecularsmoothness0",
    "custompatternspecularsmoothness1", "customprimaryalbedo", "customprimaryspecularsmoothness",
    "customscratchalbedo", "customscratchspecularsmoothness", "customsecondaryalbedo",
    "customsecondaryspecularsmoothness", "envtint", "eyeirisalbedo", "fogcolor", "foglightinscattercolor",
    "foglightoppositecolor", "glassalbedo", "glasscolor", "glasscolortint", "gunglowcolor", "gunmuzzleflashcolor",
    "hdrcolorfilter", "lasersightcolor", "pulsecolor", "shieldcolor", "ssscattering", "ssscolor", "sunlightcolor",
};

int sdk_parm_is_srgb(const char *name, size_t n)
{
    size_t i;
    for (i = 0; i < sizeof SRGB_PARMS / sizeof SRGB_PARMS[0]; i++)
        if (strlen(SRGB_PARMS[i]) == n && !_strnicmp(SRGB_PARMS[i], name, n)) return 1;
    return 0;
}

double sdk_srgb_to_linear(double x) { return sdk_srgb_decode(x); }

static int read_vec(const sdk_atom *a, double out[4], int *dims)
{
    int n = sdk_numbers(a, out, 4);
    if (n < 1) return 0;
    *dims = n;
    return 1;
}

static int sdk_value_is_form(const char *text, size_t n)
{
    static const sdk_word_id forms[] = { SDK_V_PULSE, SDK_V_FLICKER, SDK_V_FADE, SDK_V_CURVE };
    size_t i, k;
    while (n && (*text == ' ' || *text == '\t')) { text++; n--; }
    for (i = 0; i < sizeof forms / sizeof forms[0]; i++) {
        const char *w = sdk_word(forms[i]);
        size_t wn = strlen(w);
        if (n > wn && !_strnicmp(text, w, wn) && !isalnum((unsigned char)text[wn])) {
            for (k = wn; k < n && (text[k] == ' ' || text[k] == '\t'); k++) {}
            if (k < n && text[k] == '{') return 1;
        }
    }
    return 0;
}

static int dims_ok(const sdk_doc *d, size_t line, const char *field, int dims, int lo, int hi, char *why, size_t why_len)
{
    if (dims >= lo && dims <= hi) return 1;
    if (lo == hi && lo == 1)
        sdk_say(why, why_len, d, line, "'%s' takes one number, as in '%s 2'.", field, field);
    else if (lo == hi)
        sdk_say(why, why_len, d, line, "'%s' takes %d numbers, as in '%s { 1, 0.5, 0.2 }'.", field, lo, field);
    else
        sdk_say(why, why_len, d, line, "'%s' takes %d to %d numbers.", field, lo, hi);
    return 0;
}

int sdk_value_read(const sdk_doc *d, const sdk_item *it, size_t first, int dims_min, int dims_max, const char *field,
                   sdk_value *out, char *why, size_t why_len)
{
    const sdk_atom *a = first < it->count ? &it->atoms[first] : NULL;
    size_t rest = first < it->count ? it->count - first : 0, i;
    memset(out, 0, sizeof *out);
    if (!a) {
        sdk_say(why, why_len, d, it->line, "'%s' needs a value.", field);
        return 0;
    }
    if (rest == 1 && (a->kind == SDK_A_NUMBER || a->kind == SDK_A_GROUP)) {
        int dims = 0;
        if (!read_vec(a, out->c, &dims)) {
            sdk_say(why, why_len, d, it->line, "'%s' takes numbers, as in '%s { 1, 0.5, 0.2 }'.", field, field);
            return 0;
        }
        out->form = SDK_VAL_CONST;
        out->dims = dims;
        return dims_ok(d, it->line, field, dims, dims_min, dims_max, why, why_len);
    }
    if (rest == 2 && a->kind == SDK_A_WORD && it->atoms[first + 1].kind == SDK_A_GROUP) {
        const sdk_group *g = it->atoms[first + 1].group;
        int pulse = sdk_is(a, SDK_W(V_PULSE)), flick = sdk_is(a, SDK_W(V_FLICKER));
        int dims = -1;
        if (pulse || flick) {
            int have = 0;
            out->form = pulse ? SDK_VAL_PULSE : SDK_VAL_FLICKER;
            out->rate = 1;
            for (i = 0; i < g->count; i++) {
                const sdk_item *x = &g->items[i];
                double tmp[4];
                int n = 0;
                if (x->count != 2) goto shape;
                if (sdk_is(&x->atoms[0], SDK_W(F_RATE)) && sdk_numbers(&x->atoms[1], tmp, 1) == 1) { out->rate = tmp[0]; continue; }
                if ((sdk_is(&x->atoms[0], SDK_W(F_LOW)) || sdk_is(&x->atoms[0], SDK_W(F_HIGH))) && read_vec(&x->atoms[1], tmp, &n)) {
                    int hi = sdk_is(&x->atoms[0], SDK_W(F_HIGH));
                    if (dims >= 0 && n != dims) {
                        sdk_say(why, why_len, d, x->line, "'%s' and '%s' of '%s' take the same count of numbers.",
                                SDK_W(F_LOW), SDK_W(F_HIGH), field);
                        return 0;
                    }
                    dims = n;
                    memcpy(hi ? out->high : out->low, tmp, sizeof tmp);
                    have |= hi ? 2 : 1;
                    continue;
                }
                goto shape;
            }
            if (have != 3) {
                sdk_say(why, why_len, d, it->line, "'%s' of '%s' needs both '%s' and '%s', as in '%s { %s 2, %s 0.2, %s 1 }'.",
                        sdk_word(pulse ? SDK_V_PULSE : SDK_V_FLICKER), field, SDK_W(F_LOW), SDK_W(F_HIGH),
                        sdk_word(pulse ? SDK_V_PULSE : SDK_V_FLICKER), SDK_W(F_RATE), SDK_W(F_LOW), SDK_W(F_HIGH));
                return 0;
            }
            if (!(out->rate > 0)) {
                sdk_say(why, why_len, d, it->line, "'%s' needs a rate above 0 (times a second).", SDK_W(F_RATE));
                return 0;
            }
            out->dims = dims;
            return dims_ok(d, it->line, field, dims, dims_min, dims_max, why, why_len);
        }
        if (sdk_is(a, SDK_W(V_FADE))) {
            int have = 0;
            out->form = SDK_VAL_FADE;
            for (i = 0; i < g->count; i++) {
                const sdk_item *x = &g->items[i];
                double tmp[4];
                int n = 0;
                if (x->count != 2) goto shape;
                if (sdk_is(&x->atoms[0], SDK_W(F_OVER)) && sdk_numbers(&x->atoms[1], tmp, 1) == 1) { out->over = tmp[0]; continue; }
                if ((sdk_is(&x->atoms[0], SDK_W(F_FADEFROM)) || sdk_is(&x->atoms[0], SDK_W(F_FADETO))) && read_vec(&x->atoms[1], tmp, &n)) {
                    int to = sdk_is(&x->atoms[0], SDK_W(F_FADETO));
                    if (dims >= 0 && n != dims) {
                        sdk_say(why, why_len, d, x->line, "'%s' and '%s' of '%s' take the same count of numbers.",
                                SDK_W(F_FADEFROM), SDK_W(F_FADETO), field);
                        return 0;
                    }
                    dims = n;
                    memcpy(to ? out->to : out->from, tmp, sizeof tmp);
                    have |= to ? 2 : 1;
                    continue;
                }
                goto shape;
            }
            if (have != 3) {
                sdk_say(why, why_len, d, it->line, "'%s' of '%s' needs '%s' and '%s', as in '%s { %s 1, %s 0, %s 2 }'.",
                        SDK_W(V_FADE), field, SDK_W(F_FADEFROM), SDK_W(F_FADETO), SDK_W(V_FADE), SDK_W(F_FADEFROM),
                        SDK_W(F_FADETO), SDK_W(F_OVER));
                return 0;
            }
            out->dims = dims;
            return dims_ok(d, it->line, field, dims, dims_min, dims_max, why, why_len);
        }
        if (sdk_is(a, SDK_W(V_CURVE))) {
            out->form = SDK_VAL_CURVE;
            for (i = 0; i < g->count; i++) {
                const sdk_item *x = &g->items[i];
                double tmp[4];
                int n = 0;
                if (x->count == 2 && sdk_is(&x->atoms[0], SDK_W(F_LOOP))) {
                    if (sdk_is(&x->atoms[1], SDK_W(V_YES))) out->loop = 1;
                    else if (sdk_is(&x->atoms[1], SDK_W(V_NO))) out->loop = 0;
                    else goto shape;
                    continue;
                }
                if (x->count != 2 || x->atoms[0].kind != SDK_A_NUMBER || !read_vec(&x->atoms[1], tmp, &n)) goto shape;
                if (dims >= 0 && n != dims) {
                    sdk_say(why, why_len, d, x->line, "every point of '%s' takes the same count of numbers.", field);
                    return 0;
                }
                if (out->points && x->atoms[0].num <= out->t[out->points - 1]) {
                    sdk_say(why, why_len, d, x->line, "the points of a '%s' go forward in time; %g comes after %g.",
                            SDK_W(V_CURVE), x->atoms[0].num, out->t[out->points - 1]);
                    return 0;
                }
                if (out->points == SDK_CURVE_MAX) {
                    sdk_say(why, why_len, d, x->line, "a '%s' holds at most %d points.", SDK_W(V_CURVE), SDK_CURVE_MAX);
                    return 0;
                }
                dims = n;
                out->t[out->points] = x->atoms[0].num;
                memcpy(out->v[out->points], tmp, sizeof tmp);
                out->points++;
            }
            if (out->points < 2) {
                sdk_say(why, why_len, d, it->line, "a '%s' needs at least two points, as in '%s { 0 1, 1 0 }'.",
                        SDK_W(V_CURVE), SDK_W(V_CURVE));
                return 0;
            }
            if (out->t[0] < 0) {
                sdk_say(why, why_len, d, it->line, "a '%s' starts at time 0 or later.", SDK_W(V_CURVE));
                return 0;
            }
            out->dims = dims;
            return dims_ok(d, it->line, field, dims, dims_min, dims_max, why, why_len);
        }
    }
shape:
    sdk_say(why, why_len, d, it->line, "'%s' takes a number or { r, g, b }, or %s, %s, %s or %s { ... }.", field,
            SDK_W(V_PULSE), SDK_W(V_FLICKER), SDK_W(V_FADE), SDK_W(V_CURVE));
    return 0;
}

char *sdk_value_table_text(const sdk_value *v, int c, int clamp, double *lo, double *hi)
{
    sdk_buf b = { 0 };
    double l = 1e30, h = -1e30, span;
    int i;
    for (i = 0; i < v->points; i++) {
        if (v->v[i][c] < l) l = v->v[i][c];
        if (v->v[i][c] > h) h = v->v[i][c];
    }
    if (h - l < 1e-9) h = l + 1;
    span = v->t[v->points - 1] > 0 ? v->t[v->points - 1] : 1;
    *lo = l;
    *hi = h;
    sdk_printf(&b, "{ %smin 0 max 1 left 0 right 1 {", clamp ? "clamp " : "");
    for (i = 0; i < v->points; i++) {
        char t[32], x[32];
        sdk_fmt(t, sizeof t, v->t[i] / span);
        sdk_fmt(x, sizeof x, (v->v[i][c] - l) / (h - l));
        sdk_printf(&b, "%s%s:%s", i ? ", " : "", t, x);
    }
    sdk_printf(&b, "} }\n");
    if (b.oom) { free(b.p); return NULL; }
    return b.p;
}

static void lit(sdk_buf *b, const char *eol, int t, double x)
{
    char n[32];
    sdk_fmt(n, sizeof n, x);
    sdk_printf(b, "\ttemp%d\t%s%s", t, n, eol);
}

int sdk_value_material(const sdk_doc *d, size_t line, const char *key, const sdk_value *v, int *temp, sdk_buf *out,
                       const char *eol, char *why, size_t why_len)
{
    static const char MASK[4] = { 'x', 'y', 'z', 'w' };
    int srgb = sdk_parm_is_srgb(key, strlen(key)), c, t = *temp;
    int need = 3 + 4 * (v->dims > 0 ? v->dims : 1);
    double lo[4], hi[4];
    const char *table[4] = { NULL, NULL, NULL, NULL };
    double rate = 1;
    if (v->form == SDK_VAL_CONST) {
        char n[32];
        sdk_printf(out, "\t%s\t", key);
        if (v->dims == 1) { sdk_fmt(n, sizeof n, v->c[0]); sdk_printf(out, "%s", n); }
        else {
            sdk_printf(out, "{ ");
            for (c = 0; c < v->dims; c++) { sdk_fmt(n, sizeof n, v->c[c]); sdk_printf(out, "%s%s", c ? ", " : "", n); }
            sdk_printf(out, " }");
        }
        sdk_printf(out, "%s", eol);
        return 1;
    }
    if (v->form == SDK_VAL_FADE || (v->form == SDK_VAL_CURVE && !v->loop)) {
        sdk_say(why, why_len, d, line, "a material has no start, so '%s' cannot %s once; write %s, %s, or a %s with "
                "'%s %s', or fade the entity from a script.", key, SDK_W(V_FADE), SDK_W(V_PULSE), SDK_W(V_FLICKER),
                SDK_W(V_CURVE), SDK_W(F_LOOP), SDK_W(V_YES));
        return 0;
    }
    if (t + need > 128) {
        sdk_say(why, why_len, d, line, "this material changes too many values over time; split it into two materials.");
        return 0;
    }
    for (c = 0; c < v->dims; c++) {
        if (v->form == SDK_VAL_PULSE || v->form == SDK_VAL_FLICKER) {
            lo[c] = v->low[c];
            hi[c] = v->high[c];
        } else {
            double l, h;
            char *text = sdk_value_table_text(v, c, 0, &l, &h);
            if (!text) return -1;
            table[c] = sdk_register("table", text);
            free(text);
            if (!table[c]) return -1;
            lo[c] = l;
            hi[c] = h;
        }
        if (srgb) { lo[c] = sdk_srgb_to_linear(lo[c]); hi[c] = sdk_srgb_to_linear(hi[c]); }
    }
    if (v->form == SDK_VAL_CURVE) rate = 1.0 / (v->t[v->points - 1] > 0 ? v->t[v->points - 1] : 1);
    else rate = v->rate;
    /* the phase: time x rate */
    lit(out, eol, t + 1, rate);
    sdk_printf(out, "\ttemp%d\ttime * temp%d%s", t + 2, t + 1, eol);
    if (v->form == SDK_VAL_PULSE) sdk_printf(out, "\ttemp%d\tsintable[temp%d]%s", t + 3, t + 2, eol);
    else if (v->form == SDK_VAL_FLICKER) sdk_printf(out, "\ttemp%d\trandom_smooth03[temp%d]%s", t + 3, t + 2, eol);
    t += 3;
    for (c = 0; c < v->dims; c++) {
        int s = t;
        double amp, mid;
        if (v->form == SDK_VAL_CURVE) {
            sdk_printf(out, "\ttemp%d\t%s[temp%d]%s", t + 1, table[c], *temp + 2, eol);
            s = t + 1;
            t++;
        }
        if (v->form == SDK_VAL_PULSE) { amp = (hi[c] - lo[c]) / 2; mid = (hi[c] + lo[c]) / 2; } /* sintable: -1..1 */
        else { amp = hi[c] - lo[c]; mid = lo[c]; }                                        /* 0..1 tables */
        lit(out, eol, t + 1, amp);
        sdk_printf(out, "\ttemp%d\ttemp%d * temp%d%s", t + 2, s, t + 1, eol);
        lit(out, eol, t + 3, mid);
        sdk_printf(out, "\t%s.%c\ttemp%d + temp%d%s", key, MASK[c], t + 2, t + 3, eol);
        t += 3;
    }
    *temp = t;
    return 1;
}

/* The highest tempN the text names. */
static int max_temp(const char *text, size_t len)
{
    size_t i;
    int m = 0;
    for (i = 0; i + 4 < len; i++) {
        if ((i == 0 || !isalnum((unsigned char)text[i - 1])) && !_strnicmp(text + i, "temp", 4) &&
            isdigit((unsigned char)text[i + 4])) {
            int n = atoi(text + i + 4);
            if (n > m) m = n;
        }
    }
    return m;
}

int sdk_values_material_text(const char *path, const char *text, size_t len, char **out, size_t *out_len, char *why,
                             size_t why_len)
{
    sdk_buf b = { 0 };
    const char *eol = memchr(text, '\r', len) ? "\r\n" : "\n";
    size_t pos = 0, line = 1;
    int temp = max_temp(text, len), any = 0;
    *out = NULL;
    if (out_len) *out_len = 0;
    if (why && why_len) why[0] = 0;
    while (pos < len) {
        size_t s = pos, e, k, ks, kn, vs, depth = 0, end, lines = 1;
        const char *nl = (const char *)memchr(text + pos, '\n', len - pos);
        e = nl ? (size_t)(nl - text) : len;
        k = s;
        while (k < e && (text[k] == ' ' || text[k] == '\t')) k++;
        ks = k;
        while (k < e && !isspace((unsigned char)text[k]) && text[k] != '{' && text[k] != '}') k++;
        kn = k - ks;
        while (k < e && (text[k] == ' ' || text[k] == '\t')) k++;
        vs = k;
        if (kn && vs < e && sdk_value_is_form(text + vs, e - vs)) {
            /* the form's braces may run onto later lines */
            size_t q;
            end = e;
            for (q = vs; q < len; q++) {
                if (text[q] == '{') depth++;
                else if (text[q] == '}') { if (--depth == 0) { end = q + 1; break; } }
                else if (text[q] == '\n') lines++;
            }
            if (depth) {
                sdk_doc dd;
                memset(&dd, 0, sizeof dd);
                snprintf(dd.file, sizeof dd.file, "%s", path ? (strrchr(path, '/') ? strrchr(path, '/') + 1 : path) : "");
                sdk_say(why, why_len, &dd, line, "the '{' of '%.*s' is never closed.", (int)kn, text + ks);
                free(b.p);
                return 2;
            }
            {
                sdk_buf w = { 0 };
                sdk_doc doc;
                char key[96];
                size_t i;
                int r;
                for (i = 1; i < line; i++) sh_text_append(&w, "\n", 1);
                sh_text_append(&w, "{ ", 2);
                sh_text_append(&w, text + ks, end - ks);
                sh_text_append(&w, " }", 2);
                if (w.oom) { free(b.p); return -1; }
                r = sdk_parse(path, w.p, w.n, &doc, why, why_len);
                free(w.p);
                if (r <= 0) { free(b.p); return r < 0 ? -1 : 2; }
                snprintf(key, sizeof key, "%.*s", (int)kn, text + ks);
                {
                    sdk_value v;
                    const sdk_item *it = doc.root->count ? &doc.root->items[0] : NULL;
                    r = it && sdk_value_read(&doc, it, 1, 1, 4, key, &v, why, why_len);
                    if (r) r = sdk_value_material(&doc, line, key, &v, &temp, &b, eol, why, why_len);
                }
                sdk_doc_free(&doc);
                if (r < 0) { free(b.p); return -1; }
                if (!r) { free(b.p); return 2; }
            }
            any = 1;
            /* the rest of the last line after the form's '}' */
            pos = end;
            while (pos < len && text[pos] != '\n') pos++;
            if (pos < len) pos++;
            line += lines;
            continue;
        }
        sh_text_append(&b, text + s, (nl ? e + 1 : e) - s);
        pos = nl ? e + 1 : e;
        line++;
    }
    if (b.oom) { free(b.p); return -1; }
    if (!any) { free(b.p); return 0; }
    *out = b.p;
    if (out_len) *out_len = b.n;
    return 1;
}
