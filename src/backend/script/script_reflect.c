/* The reflection walk (script_reflect.h): pure, over a host's types and memory. */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "script_reflect.h"

static char *dup_n(const char *s, size_t n)
{
    char *out = (char *)malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, s, n);
    out[n] = 0;
    return out;
}

/* A copy of [b, e) without the spaces around it and without a leading "const ". */
static char *dup_trim(const char *b, const char *e)
{
    while (b < e && isspace((unsigned char)*b)) b++;
    while (e > b && isspace((unsigned char)e[-1])) e--;
    if ((size_t)(e - b) > 6 && !strncmp(b, "const ", 6)) { b += 6; while (b < e && isspace((unsigned char)*b)) b++; }
    return dup_n(b, (size_t)(e - b));
}

static void set_err(char *err, size_t cap, const char *fmt, ...)
{
    va_list ap;
    if (!err || !cap) return;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
    err[cap - 1] = 0;
}

void sh_rf_loc_free(sh_rf_loc *loc)
{
    if (!loc) return;
    free(loc->type);
    free(loc->elem_ops);
    memset(loc, 0, sizeof *loc);
}

typedef struct prim { const char *name; int kind; uint32_t size; } prim;
static const prim k_prims[] = {
    { "bool", SH_RF_BOOL, 1 },
    { "char", SH_RF_INT, 1 }, { "signed char", SH_RF_INT, 1 }, { "unsigned char", SH_RF_UINT, 1 },
    { "byte", SH_RF_UINT, 1 }, { "uint8", SH_RF_UINT, 1 }, { "int8", SH_RF_INT, 1 },
    { "short", SH_RF_INT, 2 }, { "unsigned short", SH_RF_UINT, 2 }, { "uint16", SH_RF_UINT, 2 }, { "int16", SH_RF_INT, 2 },
    { "int", SH_RF_INT, 4 }, { "unsigned int", SH_RF_UINT, 4 }, { "uint32", SH_RF_UINT, 4 }, { "int32", SH_RF_INT, 4 },
    { "long", SH_RF_INT, 4 }, { "unsigned long", SH_RF_UINT, 4 },
    { "long long", SH_RF_INT, 8 }, { "unsigned long long", SH_RF_UINT, 8 }, { "uint64", SH_RF_UINT, 8 },
    { "int64", SH_RF_INT, 8 }, { "__int64", SH_RF_INT, 8 }, { "unsigned __int64", SH_RF_UINT, 8 },
    { "float", SH_RF_FLOAT, 4 }, { "double", SH_RF_DOUBLE, 8 },
};

static const prim *prim_of(const char *type)
{
    size_t i;
    for (i = 0; i < sizeof k_prims / sizeof k_prims[0]; i++)
        if (!strcmp(k_prims[i].name, type)) return &k_prims[i];
    return NULL;
}

#define CH_LT 0x3C   /* the template brackets and separators, by number */
#define CH_GT 0x3E
#define CH_COMMA 0x2C
#define CH_STAR 0x2A
#define CH_OPEN 0x5B
#define CH_CLOSE 0x5D
#define CH_DOT 0x2E

/* "Name < a , b >": the name's end and the argument span; 0 when `type` is no template. */
static int tmpl_span(const char *type, const char **name_end, const char **args, const char **args_end)
{
    const char *lt = strchr(type, CH_LT), *gt = strrchr(type, CH_GT), *e;
    if (!lt || !gt || gt < lt) return 0;
    e = lt;
    while (e > type && isspace((unsigned char)e[-1])) e--;
    *name_end = e;
    *args = lt + 1;
    *args_end = gt;
    return 1;
}

/* The index-th top-level argument of a template's argument span, trimmed. */
static char *tmpl_arg(const char *b, const char *e, int index)
{
    const char *p, *start = b;
    int depth = 0, at = 0;
    for (p = b; p <= e; p++) {
        if (p == e || (*p == CH_COMMA && depth == 0)) {
            if (at == index) return dup_trim(start, p);
            at++;
            start = p + 1;
        } else if (*p == CH_LT) depth++;
        else if (*p == CH_GT) depth--;
    }
    return NULL;
}

/* The size of one element of type `type` (with "*" in ops: a pointer); 0 when the game does not say. */
static uint32_t elem_size(const sh_rf_host *h, const char *type, const char *ops)
{
    const prim *p;
    uint32_t size = 0;
    if (ops && strchr(ops, CH_STAR)) return 8;
    if ((p = prim_of(type)) != NULL) return p->size;
    if (!strcmp(type, "idStr")) return 0x30;
    if (h->enum_value && h->enum_value(h->ctx, type, NULL, NULL)) return 4;
    if (!strncmp(type, "idManagedClassPtr", 17)) return 32;
    if (h->class_size && h->class_size(h->ctx, type, &size)) return size;
    return 0;
}

/* Split an element type "T *" into the type (in place) and its "*". */
static int split_elem(char *arg, char **ops)
{
    size_t n = strlen(arg);
    int stars = 0;
    while (n && (arg[n - 1] == CH_STAR || isspace((unsigned char)arg[n - 1]))) { if (arg[n - 1] == CH_STAR) stars++; n--; }
    arg[n] = 0;
    *ops = dup_n(stars ? "*" : "", stars ? 1 : 0);
    return *ops != NULL;
}

int sh_rf_classify(const sh_rf_host *h, const char *type, const char *ops, uint32_t size, const uint8_t *addr,
                   sh_rf_loc *out)
{
    const char *name_end, *args, *args_end, *bracket;
    const prim *p;
    uint32_t n = 0, csize = 0;
    int stars = 0;
    const char *o;
    memset(out, 0, sizeof *out);
    out->addr = addr;
    out->size = size;
    if (!type) type = "";
    if (!ops) ops = "";
    while (!strncmp(type, "const ", 6)) type += 6;
    for (o = ops; *o; o++) if (*o == CH_STAR) stars++;
    bracket = strchr(ops, CH_OPEN);
    if (bracket) {
        n = 1;
        while (bracket) {                     /* T x[a][b]: a times b elements */
            unsigned long k = strtoul(bracket + 1, NULL, 10);
            n *= k ? (uint32_t)k : 1;
            bracket = strchr(bracket + 1, CH_OPEN);
        }
        out->kind = SH_RF_ARRAY;
        out->count = n;
        out->type = dup_n(type, strlen(type));
        out->elem_ops = dup_n(stars ? "*" : "", stars ? 1 : 0);
        if (!out->type || !out->elem_ops) { sh_rf_loc_free(out); return 0; }
        out->size = size && n ? size / n : elem_size(h, type, out->elem_ops);
        return 1;
    }
    if (stars) {
        out->kind = stars == 1 ? SH_RF_POINTER : SH_RF_OPAQUE;
        out->size = 8;
        out->type = dup_n(type, strlen(type));
        return out->type != NULL;
    }
    if (tmpl_span(type, &name_end, &args, &args_end)) {
        size_t nl = (size_t)(name_end - type);
        int is_list = (nl == 6 && !strncmp(type, "idList", 6)) || (nl == 12 && !strncmp(type, "idStaticList", 12));
        int is_array = nl == 7 && !strncmp(type, "idArray", 7);
        int is_ref = nl == 17 && !strncmp(type, "idManagedClassPtr", 17);
        if (is_list || is_array) {
            char *arg = tmpl_arg(args, args_end, 0);
            if (!arg || !split_elem(arg, &out->elem_ops)) { free(arg); sh_rf_loc_free(out); return 0; }
            out->type = arg;
            out->kind = is_list ? SH_RF_LIST : SH_RF_ARRAY;
            if (is_array) {
                char *cnt = tmpl_arg(args, args_end, 1);
                out->count = cnt ? (uint32_t)strtoul(cnt, NULL, 10) : 0;
                free(cnt);
                out->size = size && out->count ? size / out->count : elem_size(h, arg, out->elem_ops);
            } else {
                out->size = elem_size(h, arg, out->elem_ops);
            }
            return 1;
        }
        if (is_ref) {
            out->kind = SH_RF_REF;
            out->type = tmpl_arg(args, args_end, 0);
            return out->type != NULL;
        }
    }
    out->type = dup_n(type, strlen(type));
    if (!out->type) return 0;
    if ((p = prim_of(type)) != NULL) {
        out->kind = p->kind;
        if (!out->size) out->size = p->size;
    } else if (!strcmp(type, "idStr")) {
        out->kind = SH_RF_STR;
        if (!out->size) out->size = 0x30;
    } else if (h->enum_value && h->enum_value(h->ctx, type, NULL, NULL)) {
        out->kind = SH_RF_ENUM;
        if (!out->size) out->size = 4;
    } else if (h->class_size && h->class_size(h->ctx, type, &csize)) {
        out->kind = SH_RF_STRUCT;
        if (!out->size) out->size = csize;
    } else {
        out->kind = SH_RF_OPAQUE;
    }
    return 1;
}

int sh_rf_find_field(const sh_rf_host *h, const char *cls, const char *name, sh_rf_field *out)
{
    int pass;
    if (!h || !h->class_field || !cls || !name) return 0;
    for (pass = 0; pass < 2; pass++) {            /* the exact spelling first, then any case */
        const char *c = cls;
        int guard = 0;
        while (c && c[0] && guard++ < 512) {      /* an inheritance chain never loops; the guard is for bad memory */
            uint32_t i;
            sh_rf_field f;
            const char *super;
            for (i = 0; h->class_field(h->ctx, c, i, &f); i++) {
                if (!f.name) continue;
                if (pass ? !_stricmp(f.name, name) : !strcmp(f.name, name)) { *out = f; return 1; }
            }
            super = h->class_super ? h->class_super(h->ctx, c) : NULL;
            if (!super || !super[0] || !strcmp(super, c)) break;
            c = super;
        }
    }
    return 0;
}

static const char *kind_word(int kind)
{
    switch (kind) {
    case SH_RF_BOOL: return "a true or false value";
    case SH_RF_INT: case SH_RF_UINT: case SH_RF_FLOAT: case SH_RF_DOUBLE: return "a number";
    case SH_RF_ENUM: return "a named value";
    case SH_RF_STR: return "text";
    case SH_RF_LIST: case SH_RF_ARRAY: return "a list";
    case SH_RF_STRUCT: return "an object";
    default: return "a value the game does not describe";
    }
}

long long sh_rf_count(const sh_rf_host *h, const sh_rf_loc *list)
{
    int32_t n = 0;
    if (!list) return -1;
    if (list->kind == SH_RF_ARRAY) return list->count;
    if (list->kind != SH_RF_LIST || !h->read || !h->read(h->ctx, list->addr + SH_RF_LIST_NUM, &n, sizeof n) || n < 0) return -1;
    return n;
}

int sh_rf_element(const sh_rf_host *h, const sh_rf_loc *list, uint32_t index, sh_rf_loc *out, char *err, size_t err_cap)
{
    const uint8_t *base = list->addr;
    long long n;
    memset(out, 0, sizeof *out);
    if (list->kind != SH_RF_LIST && list->kind != SH_RF_ARRAY) { set_err(err, err_cap, "it is not a list"); return 0; }
    n = sh_rf_count(h, list);
    if (n < 0) { set_err(err, err_cap, "the list could not be read"); return 0; }
    if ((long long)index >= n) {
        set_err(err, err_cap, "the list holds %lld element%s, so it has no [%u]", n, n == 1 ? "" : "s", index + 1);
        return 0;
    }
    if (!list->size) { set_err(err, err_cap, "the game does not say how large one %s is", list->type ? list->type : "element"); return 0; }
    if (list->kind == SH_RF_LIST) {
        const uint8_t *p = NULL;
        if (!h->read(h->ctx, list->addr + SH_RF_LIST_PTR, (void *)&p, sizeof p) || !p) {
            set_err(err, err_cap, "the list could not be read");
            return 0;
        }
        base = p;
    }
    if (!sh_rf_classify(h, list->type, list->elem_ops, list->size, base + (size_t)index * list->size, out)) {
        set_err(err, err_cap, "there was not enough memory");
        return 0;
    }
    return 1;
}

/* Make `cur` the object a pointer or reference names, so a field can be asked of it. */
static int enter_object(const sh_rf_host *h, sh_rf_loc *cur, const char *sofar, const char *next, char *err, size_t cap)
{
    int guard = 0;
    while ((cur->kind == SH_RF_POINTER || cur->kind == SH_RF_REF) && guard++ < 64) {
        const void *obj = NULL;
        const char *cls = cur->type;
        char *owned;
        if (cur->kind == SH_RF_POINTER) {
            if (!h->read(h->ctx, cur->addr, (void *)&obj, sizeof obj)) { set_err(err, cap, "%s could not be read", sofar); return 0; }
            if (!obj) { set_err(err, cap, "%s is empty, so it has no %s", sofar, next); return 0; }
            if (h->object_class) { const char *c = h->object_class(h->ctx, obj, cur->type); if (c && c[0]) cls = c; }
        } else if (!h->follow_ref || !h->follow_ref(h->ctx, cur->addr, cur->type, &obj, &cls) || !obj) {
            set_err(err, cap, "%s names no entity now, so it has no %s", sofar, next);
            return 0;
        }
        owned = dup_n(cls, strlen(cls));
        if (!owned) { set_err(err, cap, "there was not enough memory"); return 0; }
        sh_rf_loc_free(cur);
        cur->kind = SH_RF_STRUCT;
        cur->addr = (const uint8_t *)obj;
        cur->type = owned;
    }
    return 1;
}

int sh_rf_resolve(const sh_rf_host *h, const void *object, const char *cls, const char *path, sh_rf_loc *out,
                  char *err, size_t err_cap)
{
    sh_rf_loc cur;
    const char *p = path ? path : "";
    char *sofar = NULL;
    size_t sofar_len = 0;
    int ok = 0;
    memset(out, 0, sizeof *out);
    memset(&cur, 0, sizeof cur);
    if (err && err_cap) err[0] = 0;
    if (!h || !object || !cls) { set_err(err, err_cap, "there is no object to read"); return 0; }
    cur.kind = SH_RF_STRUCT;
    cur.addr = (const uint8_t *)object;
    cur.type = dup_n(cls, strlen(cls));
    sofar = dup_n("", 0);
    if (!cur.type || !sofar) { set_err(err, err_cap, "there was not enough memory"); goto done; }
    while (*p) {
        const char *tok = p;
        char *grown;
        if (*p == CH_DOT) { p++; continue; }
        if (*p == CH_OPEN) {
            char *end = NULL;
            unsigned long index = strtoul(p + 1, &end, 10);
            sh_rf_loc next;
            if (end == p + 1 || !end || *end != CH_CLOSE) { set_err(err, err_cap, "\"%s\" has a [ without a number and ] after it", path); goto done; }
            p = end + 1;
            if (!index) { set_err(err, err_cap, "\"%s\" asks for [0]; a list's first element is [1]", path); goto done; }
            if (cur.kind != SH_RF_LIST && cur.kind != SH_RF_ARRAY) {
                set_err(err, err_cap, "%s is %s, not a list, so it has no [%lu]", sofar[0] ? sofar : "the object",
                        kind_word(cur.kind), index);
                goto done;
            }
            if (!sh_rf_element(h, &cur, (uint32_t)(index - 1), &next, err, err_cap)) {
                char why[200];
                set_err(why, sizeof why, "%s", err ? err : "");
                set_err(err, err_cap, "%s: %s", sofar, why);
                goto done;
            }
            sh_rf_loc_free(&cur);
            cur = next;
        } else {
            char name[256];
            size_t n = 0;
            sh_rf_field f;
            sh_rf_loc next;
            while (*p && *p != CH_DOT && *p != CH_OPEN) { if (n + 1 < sizeof name) name[n++] = *p; p++; }
            name[n] = 0;
            if (!enter_object(h, &cur, sofar[0] ? sofar : "the object", name, err, err_cap)) goto done;
            if (cur.kind != SH_RF_STRUCT) {
                set_err(err, err_cap, "%s is %s, so it has no %s", sofar[0] ? sofar : "the object", kind_word(cur.kind), name);
                goto done;
            }
            if (!sh_rf_find_field(h, cur.type, name, &f)) {
                set_err(err, err_cap, "%s has no field named %s", cur.type, name);
                goto done;
            }
            if (!sh_rf_classify(h, f.type, f.ops, f.size, cur.addr + f.offset, &next)) {
                set_err(err, err_cap, "there was not enough memory");
                goto done;
            }
            sh_rf_loc_free(&cur);
            cur = next;
        }
        grown = (char *)realloc(sofar, sofar_len + (size_t)(p - tok) + 1);
        if (!grown) { set_err(err, err_cap, "there was not enough memory"); goto done; }
        sofar = grown;
        memcpy(sofar + sofar_len, tok, (size_t)(p - tok));
        sofar_len += (size_t)(p - tok);
        sofar[sofar_len] = 0;
    }
    *out = cur;
    memset(&cur, 0, sizeof cur);
    ok = 1;
done:
    sh_rf_loc_free(&cur);
    free(sofar);
    return ok;
}
