/* Backend-owned SnapStack stores and console handlers. JSON span edits are
 * delegated to json_patch.c. Console dispatch runs handlers on the engine main
 * thread; frontend stack push/clear slots share the numbered stores under a lock.
 * Named groups are used only by console handlers. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <float.h>
#include <stdarg.h>
#include "snapstack.h"
#include "snapmap_plus_iface.h"
#include "json_patch.h"
#include "commands.h"
#include "backend_log.h"
#include "rawmap.h"        /* sh_rawmap_load_generation: the map the ids belong to */
#include "mkcmd_template.inc"

/* Growable ID storage shared by numbered stacks and named groups. */
typedef struct ss_ids {
    int *items;
    int  count;
    int  cap;
} ss_ids;

static int ss_ids_reserve(ss_ids *v, int need)
{
    if (need <= v->cap) return 1;
    int ncap = v->cap ? v->cap * 2 : 16;
    while (ncap < need) ncap *= 2;
    int *ni = (int *)realloc(v->items, (size_t)ncap * sizeof(int));
    if (!ni) return 0;
    v->items = ni;
    v->cap = ncap;
    return 1;
}
static int ss_ids_contains(const ss_ids *v, int id)
{
    for (int i = 0; i < v->count; i++) if (v->items[i] == id) return 1;
    return 0;
}
/* Append IDs not already present; return the number added. */
static int ss_ids_push_dedup(ss_ids *v, const int *ids, int n)
{
    int pushed = 0;
    for (int i = 0; i < n; i++) {
        if (ss_ids_contains(v, ids[i])) continue;
        if (!ss_ids_reserve(v, v->count + 1)) break;
        v->items[v->count++] = ids[i];
        pushed++;
    }
    return pushed;
}
static void ss_ids_clear(ss_ids *v) { v->count = 0; }
/* Copy IDs to caller-owned memory and empty the source; caller frees out. */
static int ss_ids_move_out(ss_ids *v, int **out)
{
    if (!v) { *out = NULL; return 0; }
    int n = v->count;
    if (n == 0) { *out = NULL; return 0; }
    int *copy = (int *)malloc((size_t)n * sizeof(int));
    if (!copy) { *out = NULL; return 0; }
    memcpy(copy, v->items, (size_t)n * sizeof(int));
    v->count = 0;
    *out = copy;
    return n;
}
/* Copy IDs to caller-owned memory without consuming the source. */
static int ss_ids_copy_out(const ss_ids *v, int **out)
{
    if (!v) { *out = NULL; return 0; }
    int n = v->count;
    if (n == 0) { *out = NULL; return 0; }
    int *copy = (int *)malloc((size_t)n * sizeof(int));
    if (!copy) { *out = NULL; return 0; }
    memcpy(copy, v->items, (size_t)n * sizeof(int));
    *out = copy;
    return n;
}
static void ss_ids_free(ss_ids *v) { free(v->items); v->items = NULL; v->count = 0; v->cap = 0; }

/* Numbered stacks. */
#define SS_MAX_STACKS 256   /* Stack indices clamp into this fixed range. */
static ss_ids g_stacks[SS_MAX_STACKS];

/* Main-thread handlers and frontend push/clear slots can mutate numbered stacks.
 * Lock mutations and snapshots so realloc cannot invalidate an active reader.
 * Plain count checks remain unlocked and can be stale; named groups are main-thread
 * only. Initialize before exposing commands or starting the frontend. */
static CRITICAL_SECTION g_ss_lock;
static volatile LONG    g_ss_lock_state = 0;   /* 0 uninit, 1 initializing, 2 ready */
static void ss_lock_init(void)
{
    LONG prev = InterlockedCompareExchange(&g_ss_lock_state, 1, 0);
    if (prev == 0) {
        InitializeCriticalSection(&g_ss_lock);
        InterlockedExchange(&g_ss_lock_state, 2);
    } else if (prev == 1) {
        while (InterlockedCompareExchange(&g_ss_lock_state, 2, 2) != 2) Sleep(0);
    }
}
static void ss_lock(void)   { ss_lock_init(); EnterCriticalSection(&g_ss_lock); }
static void ss_unlock(void) { LeaveCriticalSection(&g_ss_lock); }

static int ss_clamp_index(int index)
{
    if (index < 0) return 0;
    if (index >= SS_MAX_STACKS) return SS_MAX_STACKS - 1;   /* clamp, never index OOB */
    return index;
}
static ss_ids *stack_get(int index) { return &g_stacks[ss_clamp_index(index)]; }
/* Centralize locked stack mutations. */
static int stack_push(int index, const int *ids, int n)
{
    ss_lock();
    int pushed = ss_ids_push_dedup(stack_get(index), ids, n);
    ss_unlock();
    return pushed;
}
static void stack_clear(int index)
{
    ss_lock();
    ss_ids_clear(stack_get(index));
    ss_unlock();
}
static int stack_move_out(int index, int **out)
{
    ss_lock();
    int n = ss_ids_move_out(stack_get(index), out);
    ss_unlock();
    return n;
}

/* Named groups. */
#define SS_GROUP_NAME_CAP 64
typedef struct ss_group { char name[SS_GROUP_NAME_CAP]; ss_ids ids; } ss_group;
static ss_group *g_groups = NULL;
static int        g_group_count = 0, g_group_cap = 0;

static ss_ids *group_get(const char *name)
{
    for (int i = 0; i < g_group_count; i++)
        if (strcmp(g_groups[i].name, name) == 0) return &g_groups[i].ids;
    if (g_group_count == g_group_cap) {
        int ncap = g_group_cap ? g_group_cap * 2 : 8;
        ss_group *ni = (ss_group *)realloc(g_groups, (size_t)ncap * sizeof(ss_group));
        if (!ni) return NULL;
        g_groups = ni;
        g_group_cap = ncap;
    }
    ss_group *g = &g_groups[g_group_count++];
    memset(g, 0, sizeof *g);
    _snprintf_s(g->name, sizeof g->name, _TRUNCATE, "%s", name);
    return &g->ids;
}
static int group_has(const char *name)
{
    for (int i = 0; i < g_group_count; i++) if (strcmp(g_groups[i].name, name) == 0) return 1;
    return 0;
}
/* Replace a group's contents with these IDs. */
static void group_set(const char *name, const int *ids, int n)
{
    ss_ids *g = group_get(name);
    if (!g) return;
    ss_ids_free(g);
    if (n > 0 && ss_ids_reserve(g, n)) { memcpy(g->items, ids, (size_t)n * sizeof(int)); g->count = n; }
}
/* Remove a group and free its storage; return the previous ID count. */
static int group_remove(const char *name)
{
    for (int i = 0; i < g_group_count; i++) {
        if (strcmp(g_groups[i].name, name) != 0) continue;
        int had = g_groups[i].ids.count;
        ss_ids_free(&g_groups[i].ids);
        for (int j = i; j < g_group_count - 1; j++)
            g_groups[j] = g_groups[j + 1];
        g_group_count--;                                /* The shifted duplicate no longer owns its buffer. */
        return had;
    }
    return 0;
}

static void group_remove_all(void)
{
    for (int i = 0; i < g_group_count; i++) ss_ids_free(&g_groups[i].ids);
    g_group_count = 0;
}

static int is_valid_group_name(const char *name)
{
    /* Group names must begin with a letter. */
    return name && name[0] && isalpha((unsigned char)name[0]);
}
static int parse_stack_index(const char *arg)
{
    if (!arg) return 0;
    while (*arg == ' ' || *arg == '\t') arg++;
    if (*arg == '\0') return 0;
    int v = atoi(arg);
    return v < 0 ? 0 : v;
}
static const char *arg_at(int argc, const char **argv, int n)
{
    return (argv && n >= 0 && n < argc) ? argv[n] : NULL;
}

/* Interface operations. */
/* Use the shared vtable contract and tolerate missing slots. */

static int ic_get_selection(sh_iface *iface, int *out, int cap)
{
    if (!iface || !iface->vtbl || !iface->vtbl->get_selection) return 0;
    return iface->vtbl->get_selection(iface, out, cap);
}
static void ic_clear_selection(sh_iface *iface)
{
    if (iface && iface->vtbl && iface->vtbl->clear_selection) iface->vtbl->clear_selection(iface);
}
static void ic_add_to_selection(sh_iface *iface, int id)
{
    if (iface && iface->vtbl && iface->vtbl->add_to_selection) iface->vtbl->add_to_selection(iface, id);
}
static int ic_hovered_id(sh_iface *iface)
{
    if (!iface || !iface->vtbl || !iface->vtbl->hovered_id) return -1;
    return iface->vtbl->hovered_id(iface);
}
static int ic_is_valid_id(sh_iface *iface, int id)
{
    if (!iface || !iface->vtbl || !iface->vtbl->is_valid_id) return 0;
    return iface->vtbl->is_valid_id(iface, id) != 0;
}
static void ic_toast(sh_iface *iface, const char *title, const char *text)
{
    if (iface && iface->vtbl && iface->vtbl->toast) iface->vtbl->toast(iface, title, text);
}
static void ic_classname(sh_iface *iface, int id, char *buf, int cap)
{
    buf[0] = '\0';
    if (!iface || !iface->vtbl || !iface->vtbl->get_classname_copy) return;
    const char *s = iface->vtbl->get_classname_copy(iface, id, buf, cap);
    if (s != buf) { buf[0] = '\0'; if (s) _snprintf_s(buf, (size_t)cap, _TRUNCATE, "%s", s); }
}
static void ic_inherit(sh_iface *iface, int id, char *buf, int cap)
{
    buf[0] = '\0';
    if (!iface || !iface->vtbl || !iface->vtbl->get_inherit_copy) return;
    const char *s = iface->vtbl->get_inherit_copy(iface, id, buf, cap);
    if (s != buf) { buf[0] = '\0'; if (s) _snprintf_s(buf, (size_t)cap, _TRUNCATE, "%s", s); }
}
static void ic_declsource_text(sh_iface *iface, int id, char *buf, int cap)
{
    buf[0] = '\0';
    if (!iface || !iface->vtbl || !iface->vtbl->get_declsource_copy) return;
    const char *s = iface->vtbl->get_declsource_copy(iface, id, buf, cap);
    if (s != buf) { buf[0] = '\0'; if (s) _snprintf_s(buf, (size_t)cap, _TRUNCATE, "%s", s); }
}
static void ic_rebuild_declsource(sh_iface *iface, int id, const char *src)
{
    if (iface && iface->vtbl && iface->vtbl->rebuild_set_declsource)
        iface->vtbl->rebuild_set_declsource(iface, id, src);
}
/* Apply the final pair: 1 applied, 0 refused, -1 slot missing, -2 partial.
 * Only complete success permits a dependent source rebuild. */
static int ic_apply_class_inherit(sh_iface *iface, int id, const char *cls, const char *inh)
{
    if (!iface || !iface->vtbl || !iface->vtbl->apply_class_inherit) return -1;
    return iface->vtbl->apply_class_inherit(iface, id, cls, inh);
}
/* the id-STRING leaf value (bse/accl/acctargets/mkcmd); decimal fallback if the slot is absent/empty. */
static void ic_id_string(sh_iface *iface, int id, char *buf, int cap)
{
    buf[0] = '\0';
    if (iface && iface->vtbl && iface->vtbl->id_to_string) {
        const char *s = iface->vtbl->id_to_string(iface, id, buf, cap);
        if (s != buf && s) _snprintf_s(buf, (size_t)cap, _TRUNCATE, "%s", s);
    }
    if (buf[0] == '\0') _snprintf_s(buf, (size_t)cap, _TRUNCATE, "%d", id);
}
#define SH_APPLY_JSON_CAP (256 * 1024)   /* Per-entity serialization capacity. */
static int ic_serialize_entity(sh_iface *iface, int id, char *out, int cap)
{
    out[0] = '\0';
    if (!iface || !iface->vtbl || !iface->vtbl->serialize_entity) return 0;
    return iface->vtbl->serialize_entity(iface, id, out, cap);
}
static int ic_schedule_apply(sh_iface *iface, const sh_apply_item *items, int n, const char *op)
{
    if (!iface || !iface->vtbl || !iface->vtbl->apply_edit || n <= 0) return 0;
    return iface->vtbl->apply_edit(iface, items, n, op) != 0;
}
/* Entity JSON edits. */


static long sh_c_atoi(const char *s)
{
    if (!s) return 0;
    while (*s == ' ' || *s == '\t') s++;
    int sign = 1;
    if (*s == '+' || *s == '-') { if (*s == '-') sign = -1; s++; }
    if (!(*s >= '0' && *s <= '9')) return 0;
    long v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return sign * v;
}
static double sh_c_atof(const char *s) { return s ? atof(s) : 0.0; }

/* Format round-trip decimals with .0 for whole floats and scientific notation
 * outside exponents [-4,16), matching the engine text convention. */
static void sh_shortest_digits(double f, char *digits, int digits_cap, int *out_exp, int *out_neg)
{
    *out_neg = (f < 0.0) || (f == 0.0 && signbit(f));
    double af = fabs(f);
    char buf[64];
    int prec;
    for (prec = 0; prec <= 17; prec++) {
        _snprintf_s(buf, sizeof buf, _TRUNCATE, "%.*e", prec, af);
        if (atof(buf) == af) break;
    }
    const char *e = strchr(buf, 'e');
    size_t mant_len = e ? (size_t)(e - buf) : strlen(buf);
    *out_exp = e ? atoi(e + 1) : 0;
    int dn = 0;
    for (size_t i = 0; i < mant_len && dn < digits_cap - 1; i++)
        if (buf[i] != '.') digits[dn++] = buf[i];
    while (dn > 1 && digits[dn - 1] == '0') dn--;   /* drop trailing zeros (keep >=1 digit) */
    digits[dn] = '\0';
}
static void render_engine_float(double f, char *out, int outcap)
{
    if (f == 0.0) { _snprintf_s(out, (size_t)outcap, _TRUNCATE, "%s", signbit(f) ? "-0.0" : "0.0"); return; }
    if (isnan(f)) { _snprintf_s(out, (size_t)outcap, _TRUNCATE, "NaN"); return; }
    if (!isfinite(f)) { _snprintf_s(out, (size_t)outcap, _TRUNCATE, "%s", f > 0 ? "Infinity" : "-Infinity"); return; }

    char digits[32]; int E, neg;
    sh_shortest_digits(f, digits, sizeof digits, &E, &neg);
    int dlen = (int)strlen(digits);
    char body[64];
    if (E < -4 || E >= 16) {
        char mant[40];
        if (dlen > 1) _snprintf_s(mant, sizeof mant, _TRUNCATE, "%c.%s", digits[0], digits + 1);
        else _snprintf_s(mant, sizeof mant, _TRUNCATE, "%s", digits);
        int ae = E < 0 ? -E : E;
        _snprintf_s(body, sizeof body, _TRUNCATE, "%se%c%02d", mant, E < 0 ? '-' : '+', ae);
    } else if (E >= 0) {
        if (dlen <= E + 1) {
            char zeros[40] = {0};
            for (int i = 0; i < E + 1 - dlen && i < (int)sizeof(zeros) - 1; i++) zeros[i] = '0';
            _snprintf_s(body, sizeof body, _TRUNCATE, "%s%s.0", digits, zeros);
        } else {
            _snprintf_s(body, sizeof body, _TRUNCATE, "%.*s.%s", E + 1, digits, digits + E + 1);
        }
    } else {
        char zeros[40] = {0};
        int nz = -E - 1;
        for (int i = 0; i < nz && i < (int)sizeof(zeros) - 1; i++) zeros[i] = '0';
        _snprintf_s(body, sizeof body, _TRUNCATE, "0.%s%s", zeros, digits);
    }
    /* C-style exponent strip: e+07 -> e7, e-07 -> e-7 (drop '+' + leading zeros, keep >=1 digit). */
    char *ep = strchr(body, 'e');
    if (ep) {
        char head[48]; size_t hlen = (size_t)(ep - body) + 1;
        memcpy(head, body, hlen); head[hlen] = '\0';
        const char *tail = ep + 1;
        int em = 0; size_t k = 0;
        if (tail[k] == '+' || tail[k] == '-') { em = (tail[k] == '-'); k++; }
        while (tail[k] == '0' && k + 1 < strlen(tail)) k++;
        _snprintf_s(out, (size_t)outcap, _TRUNCATE, "%s%s%s%s", neg ? "-" : "", head, em ? "-" : "", tail + k);
        return;
    }
    _snprintf_s(out, (size_t)outcap, _TRUNCATE, "%s%s", neg ? "-" : "", body);
}

/* Encode a typed JSON leaf: string, integer, float32-rounded decimal, or boolean. */
static int encode_bulkset_leaf_json(const char *op, const char *value, char *out, int outcap)
{
    if (strcmp(op, "bsi") == 0) {
        _snprintf_s(out, (size_t)outcap, _TRUNCATE, "%ld", sh_c_atoi(value));
        return 1;
    }
    if (strcmp(op, "bsf") == 0) {
        float f32 = (float)sh_c_atof(value);            /* Narrow to the engine float width before formatting. */
        render_engine_float((double)f32, out, outcap);
        return 1;
    }
    if (strcmp(op, "bsb") == 0) {
        _snprintf_s(out, (size_t)outcap, _TRUNCATE, "%s", (strcmp(value, "true") == 0) ? "true" : "false");
        return 1;
    }
    return sh_json_quote_string(value, out, outcap);      /* bss / default: a JSON string literal */
}

/* Apply kind 0 edits and return how many the engine completed, or
 * SH_APPLY_IN_PROGRESS when it is still running. The backend reports the
 * result itself (one toast with the true count). An interface without the
 * synchronous slot schedules the edits and returns SH_APPLY_IN_PROGRESS;
 * -1 = nothing could run. */
static int ic_apply(sh_iface *iface, const sh_apply_item *items, int n, const char *op)
{
    if (n <= 0 || !iface || !iface->vtbl) return -1;
    if (iface->vtbl->apply_sync) return iface->vtbl->apply_sync(iface, items, n, op);
    return ic_schedule_apply(iface, items, n, op) ? SH_APPLY_IN_PROGRESS : -1;
}

/* ---- the open map ------------------------------------------------------------------------
 * Stacks and groups hold entity ids, which name entities of the map that was open when they
 * were pushed. Every map parse moves the load generation (rawmap.h), so the first use after a
 * different map opened empties them: an id never reaches an entity of another map. Stacks are
 * shared with the frontend under the lock; groups are touched by console handlers only. */
static unsigned long g_ss_stack_generation, g_ss_group_generation;
static int g_ss_generation_known;

static void ss_sync_stacks(void)
{
    unsigned long now = sh_rawmap_load_generation();
    ss_lock();
    if (!g_ss_generation_known) {
        g_ss_stack_generation = g_ss_group_generation = now;
        g_ss_generation_known = 1;
    } else if (g_ss_stack_generation != now) {
        for (int i = 0; i < SS_MAX_STACKS; i++) ss_ids_clear(&g_stacks[i]);
        g_ss_stack_generation = now;
    }
    ss_unlock();
}

/* Console handlers: stacks and groups. */
static void ss_sync(void)
{
    ss_sync_stacks();
    if (g_ss_group_generation != g_ss_stack_generation) {
        group_remove_all();
        g_ss_group_generation = g_ss_stack_generation;
    }
}

/* Bulk edits change at most this many entities in one command; more is refused before anything
 * changes, with the ids left where they were. */
#define SS_BULK_MAX 512

/* The operand's ids, caller-owned: a group is copied; a stack is consumed and its index is
 * written to *stack (-1 for a group), so a refused command can put them back. */
static int ss_take(int argc, const char **argv, int **out, int *stack)
{
    const char *arg = arg_at(argc, argv, 1);
    *stack = -1;
    if (arg && isalpha((unsigned char)arg[0]) && group_has(arg))
        return ss_ids_copy_out(group_get(arg), out);
    *stack = ss_clamp_index(parse_stack_index(arg));
    return stack_move_out(*stack, out);
}

static void ss_put_back(int stack, const int *ids, int n)
{
    if (stack >= 0 && n > 0) stack_push(stack, ids, n);
}

static void ss_toastf(sh_iface *iface, const char *fmt, ...)
{
    char text[256];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(text, sizeof text, _TRUNCATE, fmt, ap);
    va_end(ap);
    ic_toast(iface, "SnapStack", text);
}

/* Refuse an operand over the bulk limit, putting a consumed stack back. 1 = refused. */
static int ss_over_limit(sh_iface *iface, const char *op, int *ids, int n, int stack)
{
    if (n <= SS_BULK_MAX) return 0;
    ss_put_back(stack, ids, n);
    free(ids);
    ss_toastf(iface, "%s changes at most %d entities at once and this has %d; split it and run it again. "
              "Nothing was changed.", op, SS_BULK_MAX, n);
    return 1;
}

/* One bulk edit: serialize each entity, set `prop` to the JSON `leaf`, and apply the batch.
 * Entities without the property are counted in *missing. */
static void ss_bulk_set(sh_iface *iface, const char *op, const int *ids, int n, const char *prop, const char *leaf)
{
    char *full = (char *)malloc(SH_APPLY_JSON_CAP);
    char *scratch = (char *)malloc(SH_APPLY_JSON_CAP + 1024);
    sh_apply_item *items = (sh_apply_item *)calloc((size_t)(n > 0 ? n : 1), sizeof *items);
    char **owned = (char **)calloc((size_t)(n > 0 ? n : 1), sizeof *owned);
    int m = 0, missing = 0;

    if (!full || !scratch || !items || !owned) {
        ss_toastf(iface, "%s: not enough memory; nothing was changed.", op);
        goto done;
    }
    for (int i = 0; i < n; i++) {
        size_t plen;
        if (!ic_serialize_entity(iface, ids[i], full, SH_APPLY_JSON_CAP) ||
            !sh_json_patch_set_leaf(full, prop, leaf, scratch, SH_APPLY_JSON_CAP + 1024)) {
            missing++;
            continue;
        }
        plen = strlen(scratch);
        owned[m] = (char *)malloc(plen + 1);
        if (!owned[m]) { missing++; continue; }
        memcpy(owned[m], scratch, plen + 1);
        items[m].kind = 0; items[m].id = ids[i]; items[m].text = owned[m];
        m++;
    }
    if (m == 0) {
        ss_toastf(iface, "%s: none of the entities has the property %s; nothing was changed.", op, prop);
        goto done;
    }
    if (missing)
        ss_toastf(iface, "%s: %d of the entities have no property %s and were skipped.", op, missing, prop);
    if (ic_apply(iface, items, m, op) == -1)
        ss_toastf(iface, "%s: the editor did not accept the change; open the map in the editor and try again.", op);
done:
    if (owned) for (int i = 0; i < m; i++) free(owned[i]);
    free(owned); free(items); free(full); free(scratch);
}

/* Stack and group commands. */

/* Push selected IDs with deduplication, then clear the selection. */
static void h_psel(void *ctx, int argc, const char **argv)
{
    sh_iface *iface = (sh_iface *)ctx;
    int index = ss_clamp_index(parse_stack_index(arg_at(argc, argv, 1)));
    enum { SEL_CAP = 4096 };
    ss_sync();
    int *sel = (int *)malloc((size_t)SEL_CAP * sizeof(int));
    int *valid = (int *)malloc((size_t)SEL_CAP * sizeof(int));
    if (!sel || !valid) { free(sel); free(valid); ic_toast(iface, "SnapStack", "psel: not enough memory."); return; }
    int sel_n = ic_get_selection(iface, sel, SEL_CAP);
    int valid_n = 0;
    for (int i = 0; i < sel_n; i++)
        if (ic_is_valid_id(iface, sel[i])) valid[valid_n++] = sel[i];
    int pushed = stack_push(index, valid, valid_n);
    int now = stack_get(index)->count;
    free(sel);
    free(valid);
    ic_clear_selection(iface);
    ss_toastf(iface, "Pushed %d selected entities onto stack %d (now %d).", pushed, index, now);
}

/* Add stored IDs to the selection; consume stacks and preserve named groups. */
static void h_popsel(void *ctx, int argc, const char **argv)
{
    sh_iface *iface = (sh_iface *)ctx;
    int *ids = NULL, stack;
    ss_sync();
    int n = ss_take(argc, argv, &ids, &stack);
    for (int i = 0; i < n; i++) ic_add_to_selection(iface, ids[i]);
    free(ids);
}

/* Push the hovered entity and report the result, including a missing hover. */
static void h_phov(void *ctx, int argc, const char **argv)
{
    sh_iface *iface = (sh_iface *)ctx;
    int index = ss_clamp_index(parse_stack_index(arg_at(argc, argv, 1)));
    ss_sync();
    int hovered = ic_hovered_id(iface);
    if (hovered < 0) { ic_toast(iface, "SnapStack", "phov: nothing hovered (point at an entity in the 3D view)"); return; }
    int pushed = stack_push(index, &hovered, 1);
    int now = stack_get(index)->count;
    char idstr[200]; ic_id_string(iface, hovered, idstr, (int)sizeof idstr);
    ss_toastf(iface, "phov: %s id %d \"%s\" onto stack %d (now %d)",
              pushed ? "pushed" : "already on --", hovered, idstr, index, now);
}

/* Clear a numbered stack and report its prior count. */
static void h_cstk(void *ctx, int argc, const char **argv)
{
    sh_iface *iface = (sh_iface *)ctx;
    int index = ss_clamp_index(parse_stack_index(arg_at(argc, argv, 1)));
    ss_sync();
    int had = sh_snapstack_clear_stack_backend(index);
    ss_toastf(iface, "cstk: cleared stack %d (%d id(s) removed)", index, had);
}

/* Push valid IDs in the inclusive range, deduplicating existing entries. */
static void h_pr(void *ctx, int argc, const char **argv)
{
    sh_iface *iface = (sh_iface *)ctx;
    int index = ss_clamp_index(parse_stack_index(arg_at(argc, argv, 1)));
    const char *lo_s = arg_at(argc, argv, 2);
    const char *hi_s = arg_at(argc, argv, 3);
    int lo = lo_s ? atoi(lo_s) : 0;
    int hi = hi_s ? atoi(hi_s) : 0;
    enum { RANGE_CAP = 65536 };
    ss_sync();
    if (hi < lo) { int t = lo; lo = hi; hi = t; }
    int *in_range = (int *)malloc((size_t)RANGE_CAP * sizeof(int));
    if (!in_range) { ic_toast(iface, "SnapStack", "pr: not enough memory."); return; }
    int n = 0;
    for (int id = lo; id <= hi && n < RANGE_CAP; id++)
        if (ic_is_valid_id(iface, id)) in_range[n++] = id;
    int pushed = stack_push(index, in_range, n);
    free(in_range);
    ss_toastf(iface, "Pushed %d of the %d entities in %d-%d onto stack %d.", pushed, n, lo, hi, index);
}

/* pg <stack> <group>, or pg <group> for stack 0: push a group's ids onto a stack. */
static void h_pg(void *ctx, int argc, const char **argv)
{
    sh_iface *iface = (sh_iface *)ctx;
    const char *a1 = arg_at(argc, argv, 1);
    const char *a2 = arg_at(argc, argv, 2);
    int index;
    const char *name;
    ss_sync();
    if (a1 && isalpha((unsigned char)a1[0]) && !a2) { index = 0; name = a1; }
    else { index = ss_clamp_index(parse_stack_index(a1)); name = a2 ? a2 : ""; }
    if (!group_has(name)) { ss_toastf(iface, "pg: no group named '%s'.", name); return; }
    int *ids = NULL;
    int n = ss_ids_copy_out(group_get(name), &ids);
    int pushed = stack_push(index, ids, n);
    free(ids);
    ss_toastf(iface, "Pushed %d entities from group %s onto stack %d.", pushed, name, index);
}

/* Consume a stack into a group. A single letter-first argument implies stack 0. */
static void h_pop2g(void *ctx, int argc, const char **argv)
{
    sh_iface *iface = (sh_iface *)ctx;
    const char *a1 = arg_at(argc, argv, 1);
    const char *a2 = arg_at(argc, argv, 2);
    int index;
    const char *name;
    ss_sync();
    if (a1 && isalpha((unsigned char)a1[0]) && !a2) {   /* single letter-first arg -> group, stack 0 */
        index = 0;
        name = a1;
    } else {
        index = ss_clamp_index(parse_stack_index(a1));
        name = a2 ? a2 : "";
    }
    if (!is_valid_group_name(name)) {
        ss_toastf(iface, "pop2g: '%s' is not a group name; group names must start with a letter.", name);
        return;
    }
    int *ids = NULL;
    int n = stack_move_out(index, &ids);
    group_set(name, ids, n);
    free(ids);
    ss_toastf(iface, "pop2g: moved %d id(s) from stack %d into group '%s' (stack %d now empty)",
              n, index, name, index);
}

/* Keep IDs matching the requested class or inherit and report the survivors. */
static void do_filt(sh_iface *iface, int index, const char *match, int by_class)
{
    int *cur = NULL;
    ss_sync();
    int n = stack_move_out(index, &cur);
    int *kept = (n > 0) ? (int *)malloc((size_t)n * sizeof(int)) : NULL;
    int kn = 0;
    if (n > 0 && !kept) { ss_put_back(index, cur, n); free(cur); ic_toast(iface, "SnapStack", "Not enough memory."); return; }
    for (int i = 0; i < n; i++) {
        char field[256];
        if (by_class) ic_classname(iface, cur[i], field, (int)sizeof field);
        else          ic_inherit(iface, cur[i], field, (int)sizeof field);
        if (strcmp(field, match) == 0) kept[kn++] = cur[i];
    }
    stack_push(index, kept, kn);
    free(cur);
    free(kept);
    ss_toastf(iface, "%d entities on stack %d had %s %s", kn, index, by_class ? "class" : "inherit", match);
}
static void h_filtinh(void *ctx, int argc, const char **argv)
{
    sh_iface *iface = (sh_iface *)ctx;
    int index = ss_clamp_index(parse_stack_index(arg_at(argc, argv, 1)));
    const char *match = arg_at(argc, argv, 2);
    do_filt(iface, index, match ? match : "", 0);
}
static void h_filtcls(void *ctx, int argc, const char **argv)
{
    sh_iface *iface = (sh_iface *)ctx;
    int index = ss_clamp_index(parse_stack_index(arg_at(argc, argv, 1)));
    const char *match = arg_at(argc, argv, 2);
    do_filt(iface, index, match ? match : "", 1);
}

/* bss / bsi / bsf / bsb <stack|group> <path> <value>: set one property on every entity. */
static void do_bulkset(sh_iface *iface, const char *op, int argc, const char **argv)
{
    const char *prop = arg_at(argc, argv, 2);
    const char *val  = arg_at(argc, argv, 3);
    char leaf[64];
    int *ids = NULL, stack;
    ss_sync();
    if (!prop || !val) {
        ss_toastf(iface, strcmp(op, "bsb") == 0 ? "usage: bsb <stack> <path> true|false" : "usage: %s <stack> <path> <value>", op);
        return;
    }
    if (!encode_bulkset_leaf_json(op, val, leaf, (int)sizeof leaf)) {
        ss_toastf(iface, "%s: that value cannot be written to a property; nothing was changed.", op);
        return;
    }
    int n = ss_take(argc, argv, &ids, &stack);
    if (n <= 0) { free(ids); ss_toastf(iface, "%s: no entities on the stack.", op); return; }
    if (ss_over_limit(iface, op, ids, n, stack)) return;
    ss_bulk_set(iface, op, ids, n, prop, leaf);
    free(ids);
}
static void h_bss(void *ctx, int argc, const char **argv) { do_bulkset((sh_iface *)ctx, "bss", argc, argv); }
static void h_bsi(void *ctx, int argc, const char **argv) { do_bulkset((sh_iface *)ctx, "bsi", argc, argv); }
static void h_bsf(void *ctx, int argc, const char **argv) { do_bulkset((sh_iface *)ctx, "bsf", argc, argv); }
static void h_bsb(void *ctx, int argc, const char **argv) { do_bulkset((sh_iface *)ctx, "bsb", argc, argv); }

/* Take a stack of at least two ids: the targets, then the receiver last. Returns the count with
 * *ids owned by the caller, or 0 after telling the player (the stack is left as it was). */
static int ss_take_targets(sh_iface *iface, const char *op, int index, int **ids)
{
    int n = stack_move_out(index, ids);
    if (n >= 2) return n;
    ss_put_back(index, *ids, n);
    free(*ids);
    *ids = NULL;
    ss_toastf(iface, "%s needs at least two entities on the stack: the targets, then the receiver last.", op);
    return 0;
}

/* bse <stack> <path>: set each earlier entity's property to the last entity's reference. */
static void h_bse(void *ctx, int argc, const char **argv)
{
    sh_iface *iface = (sh_iface *)ctx;
    const char *prop = arg_at(argc, argv, 2);
    int index = ss_clamp_index(parse_stack_index(arg_at(argc, argv, 1)));
    int *ids = NULL;
    ss_sync();
    if (!prop) { ic_toast(iface, "SnapStack", "usage: bse <stack> <path>"); return; }
    int n = ss_take_targets(iface, "bse", index, &ids);
    if (n == 0) return;
    if (ss_over_limit(iface, "bse", ids, n, index)) return;
    char receiver[256]; ic_id_string(iface, ids[n - 1], receiver, (int)sizeof receiver);
    char leaf[300];
    if (!sh_json_quote_string(receiver, leaf, (int)sizeof leaf)) {
        ss_put_back(index, ids, n); free(ids);
        ic_toast(iface, "SnapStack", "bse: that entity's reference cannot be written; nothing was changed.");
        return;
    }
    ss_bulk_set(iface, "bse", ids, n - 1, prop, leaf);
    free(ids);
}

/* accl <stack> <path> / acctargets <stack>: add the earlier entities as references on the last one. */
static void do_acc(sh_iface *iface, const char *op, int argc, const char **argv, int hardcoded_targets)
{
    const char *userPath = arg_at(argc, argv, 2);
    int index = ss_clamp_index(parse_stack_index(arg_at(argc, argv, 1)));
    int *ids = NULL;
    ss_sync();
    if (!hardcoded_targets && !userPath) { ic_toast(iface, "SnapStack", "usage: accl <stack> <path>"); return; }
    int n = ss_take_targets(iface, op, index, &ids);
    if (n == 0) return;
    if (ss_over_limit(iface, op, ids, n, index)) return;
    int receiver = ids[n - 1];
    int targets = n - 1;
    const char *path = hardcoded_targets ? "targets" : userPath;

    const char **idstr_ptrs = (const char **)malloc((size_t)targets * sizeof(char *));
    char *idstr_bufs = (char *)malloc((size_t)targets * 256);
    char *full = (char *)malloc(SH_APPLY_JSON_CAP);
    char *patched = (char *)malloc(SH_APPLY_JSON_CAP + 4096);
    int nvalid = 0, skipped = 0;
    if (!idstr_ptrs || !idstr_bufs || !full || !patched) {
        ss_put_back(index, ids, n);
        ss_toastf(iface, "%s: not enough memory; nothing was changed.", op);
        goto done;
    }
    for (int i = 0; i < targets; i++) {
        if (!ic_is_valid_id(iface, ids[i])) { skipped++; continue; }
        char *slot = idstr_bufs + (size_t)nvalid * 256;
        ic_id_string(iface, ids[i], slot, 256);
        idstr_ptrs[nvalid++] = slot;
    }
    if (nvalid == 0) {
        ss_toastf(iface, "%s: all %d targets on the stack were deleted; nothing was changed.", op, targets);
        goto done;
    }
    if (!ic_serialize_entity(iface, receiver, full, SH_APPLY_JSON_CAP)) {
        ss_toastf(iface, "%s: the receiving entity could not be read; nothing was changed.", op);
        goto done;
    }
    if (!sh_json_patch_upsert_reflist(full, path, idstr_ptrs, nvalid, patched, SH_APPLY_JSON_CAP + 4096)) {
        ss_toastf(iface, "%s: the receiving entity's %s is not a list; nothing was changed.", op, path);
        goto done;
    }
    {
        sh_apply_item it; it.kind = 0; it.id = receiver; it.text = patched;
        int applied = ic_apply(iface, &it, 1, op);
        char rcv[200]; ic_id_string(iface, receiver, rcv, (int)sizeof rcv);
        if (applied == 1)
            ss_toastf(iface, "%s: added %d %s to \"%s\"%s.", op, nvalid, hardcoded_targets ? "target(s)" : "reference(s)",
                      rcv, skipped ? " (deleted ones skipped)" : "");
        else if (applied == SH_APPLY_IN_PROGRESS)
            ss_toastf(iface, "%s: the change to \"%s\" is still running in the editor.", op, rcv);
        else
            ss_toastf(iface, "%s: the editor did not accept the change to \"%s\"; nothing was changed.", op, rcv);
    }
done:
    free(idstr_ptrs); free(idstr_bufs); free(full); free(patched); free(ids);
}
static void h_accl(void *ctx, int argc, const char **argv)       { do_acc((sh_iface *)ctx, "accl", argc, argv, 0); }
static void h_acctargets(void *ctx, int argc, const char **argv) { do_acc((sh_iface *)ctx, "acctargets", argc, argv, 1); }

/* Expand each ID into the command template, retaining a trailing semicolon,
 * and stage the resulting command-entity prefab as kind 1. */
static void h_mkcmd(void *ctx, int argc, const char **argv)
{
    sh_iface *iface = (sh_iface *)ctx;
    int *ids = NULL, stack;
    ss_sync();
    int n = ss_take(argc, argv, &ids, &stack);
    if (n <= 0) { free(ids); ic_toast(iface, "SnapStack", "mkcmd: no entities on the stack."); return; }
    const char *tmpl_arg = arg_at(argc, argv, 2);
    const char *tmpl = (tmpl_arg && tmpl_arg[0]) ? tmpl_arg : "$";

    char *commandText = (char *)malloc(SH_APPLY_JSON_CAP);
    char *prefab = (char *)malloc(SH_APPLY_JSON_CAP + 4096);
    if (!commandText || !prefab) {
        ss_put_back(stack, ids, n);
        free(commandText); free(prefab); free(ids);
        ic_toast(iface, "SnapStack", "mkcmd: not enough memory; nothing was changed.");
        return;
    }
    size_t clen = 0;
    int fits = 1;
    commandText[0] = '\0';
    for (int i = 0; i < n && fits; i++) {
        char idStr[256]; ic_id_string(iface, ids[i], idStr, (int)sizeof idStr);
        char part[512]; size_t plen = 0;
        int wrote = _snprintf_s(part, sizeof part, _TRUNCATE, "ai_ScriptCmdEnt ");
        plen = (wrote > 0) ? (size_t)wrote : 0;
        for (const char *c = tmpl; *c && plen + 1 < sizeof part; c++) {
            if (*c == '$') { size_t idlen = strlen(idStr); if (plen + idlen + 1 >= sizeof part) break; memcpy(part + plen, idStr, idlen); plen += idlen; }
            else part[plen++] = *c;
        }
        part[plen] = '\0';
        if (clen + plen + 2 >= SH_APPLY_JSON_CAP) { fits = 0; break; }
        memcpy(commandText + clen, part, plen); clen += plen;
        commandText[clen++] = ';';
    }
    commandText[clen] = '\0';

    const char *ph = "__SH_MKCMD_COMMANDTEXT__";
    const char *tmplsrc = SH_MKCMD_PREFAB_TEMPLATE_C;
    const char *pos = strstr(tmplsrc, ph);
    size_t pre = pos ? (size_t)(pos - tmplsrc) : 0;
    size_t phlen = strlen(ph);
    if (!pos) {
        backend_log("SNAPSTACK: mkcmd template has no command placeholder");
        fits = 0;
    } else if (pre + clen + strlen(tmplsrc + pre + phlen) + 1 > (size_t)(SH_APPLY_JSON_CAP + 4096)) {
        fits = 0;
    }
    if (!fits) {
        ss_put_back(stack, ids, n);
        free(commandText); free(prefab); free(ids);
        ic_toast(iface, "SnapStack", "mkcmd: too many entities for one command entity; split the stack. Nothing was changed.");
        return;
    }
    memcpy(prefab, tmplsrc, pre);
    memcpy(prefab + pre, commandText, clen);
    _snprintf_s(prefab + pre + clen, (size_t)(SH_APPLY_JSON_CAP + 4096) - pre - clen, _TRUNCATE, "%s", tmplsrc + pre + phlen);
    free(commandText);
    free(ids);

    /* Defer kind 1 staging through the engine command buffer; the backend reports the result. */
    sh_apply_item it; it.kind = 1; it.id = 0; it.text = prefab;
    int ok = ic_schedule_apply(iface, &it, 1, "mkcmd");
    free(prefab);
    if (!ok) ic_toast(iface, "SnapStack", "mkcmd: the editor did not accept the command entity; open the map in the editor and try again.");
}

/* Class and inherit edits: bscls <stack> <class>, bsin <stack> <inherit>, bsincls <stack>
 * <inherit> <class>. Each entity's final pair is checked and written together (the engine
 * faults on a class that does not derive from its inherit's type, so a refused pair changes
 * nothing), then its decl source is rebuilt and the pair written again so appended old
 * source cannot win. An entity whose second write did not hold needs the command again. */
static void do_class_inherit(sh_iface *iface, const char *op, int argc, const char **argv,
                             const char *cls, const char *inh)
{
    int *ids = NULL, stack;
    int completed = 0, refused = 0, again = 0;
    ss_sync();
    int n = ss_take(argc, argv, &ids, &stack);
    if (n <= 0) { free(ids); ss_toastf(iface, "%s: no entities on the stack.", op); return; }
    if (ss_over_limit(iface, op, ids, n, stack)) return;
    for (int i = 0; i < n; i++) {
        if (!iface || !iface->vtbl || !iface->vtbl->apply_class_inherit ||
            !iface->vtbl->get_declsource_copy || !iface->vtbl->rebuild_set_declsource) {
            refused++;
            continue;
        }
        char *rsrc = (char *)malloc(64 * 1024);
        if (!rsrc) { refused++; continue; }
        int r = ic_apply_class_inherit(iface, ids[i], cls, inh);
        if (r == 1) {
            ic_declsource_text(iface, ids[i], rsrc, 64 * 1024);
            ic_rebuild_declsource(iface, ids[i], rsrc);
            if (ic_apply_class_inherit(iface, ids[i], cls, inh) == 1) completed++;
            else again++; /* Source rebuild and the first pair already ran. */
        } else if (r == 0 || r == -1) refused++;
        else again++;
        free(rsrc);
    }
    free(ids);
    if (again)
        ss_toastf(iface, "%s: %d changed, %d refused, %d need the command again (of %d entities).",
                  op, completed, refused, again, n);
    else
        ss_toastf(iface, "%s: %d changed, %d refused (of %d entities).", op, completed, refused, n);
}
static void h_bscls(void *ctx, int argc, const char **argv)
{
    const char *cls = arg_at(argc, argv, 2);
    if (!cls || !cls[0]) { ic_toast((sh_iface *)ctx, "SnapStack", "usage: bscls <stack> <className>"); return; }
    do_class_inherit((sh_iface *)ctx, "bscls", argc, argv, cls, NULL);
}
static void h_bsin(void *ctx, int argc, const char **argv)
{
    const char *inh = arg_at(argc, argv, 2);
    if (!inh || !inh[0]) { ic_toast((sh_iface *)ctx, "SnapStack", "usage: bsin <stack> <inherit>"); return; }
    do_class_inherit((sh_iface *)ctx, "bsin", argc, argv, NULL, inh);
}
static void h_bsincls(void *ctx, int argc, const char **argv)
{
    const char *inh = arg_at(argc, argv, 2);
    const char *cls = arg_at(argc, argv, 3);
    if (!inh || !inh[0] || !cls || !cls[0]) { ic_toast((sh_iface *)ctx, "SnapStack", "usage: bsincls <stack> <inherit> <className>"); return; }
    do_class_inherit((sh_iface *)ctx, "bsincls", argc, argv, cls, inh);
}

/* Store inspection and removal. */

/* chkstk [N]: N given -> list stack N's ids + count; omitted -> summarize every non-empty stack. */
static void h_chkstk(void *ctx, int argc, const char **argv)
{
    sh_iface *iface = (sh_iface *)ctx;
    const char *arg = arg_at(argc, argv, 1);
    ss_sync();
    if (arg && arg[0]) {
        int index = ss_clamp_index(parse_stack_index(arg));
        /* Snapshot under the lock before engine reads; frontend pushes may reallocate. */
        int *ids = NULL;
        ss_lock();
        int n = ss_ids_copy_out(stack_get(index), &ids);
        ss_unlock();
        sh_printf("chkstk: stack %d holds %d id(s):\n", index, n);
        for (int i = 0; i < n; i++) {
            char idstr[256]; ic_id_string(iface, ids[i], idstr, (int)sizeof idstr);
            sh_printf("  [%d] id=%d  \"%s\"\n", i, ids[i], idstr);
        }
        free(ids);
        ss_toastf(iface, "stack %d: %d id(s)", index, n);
    } else {
        int nonempty = 0, total = 0;
        sh_printf("chkstk: non-empty stacks:\n");
        ss_lock();
        for (int i = 0; i < SS_MAX_STACKS; i++) {
            if (g_stacks[i].count == 0) continue;
            sh_printf("  stack %d: %d id(s)\n", i, g_stacks[i].count);
            nonempty++; total += g_stacks[i].count;
        }
        ss_unlock();
        if (nonempty == 0) sh_printf("  (all %d stacks empty)\n", SS_MAX_STACKS);
        ss_toastf(iface, "%d non-empty stack(s), %d id(s) total", nonempty, total);
    }
}

/* chkgrp [name]: name given -> list that group's ids + count; omitted -> list all group names + counts. */
static void h_chkgrp(void *ctx, int argc, const char **argv)
{
    sh_iface *iface = (sh_iface *)ctx;
    const char *name = arg_at(argc, argv, 1);
    ss_sync();
    if (name && name[0]) {
        if (!group_has(name)) {
            char t[128]; _snprintf_s(t, sizeof t, _TRUNCATE, "chkgrp: no group named '%s'", name);
            sh_printf("%s\n", t); ic_toast(iface, "SnapStack", t); return;
        }
        ss_ids *g = group_get(name);
        sh_printf("chkgrp: group '%s' holds %d id(s):\n", name, g->count);
        for (int i = 0; i < g->count; i++) {
            char idstr[256]; ic_id_string(iface, g->items[i], idstr, (int)sizeof idstr);
            sh_printf("  [%d] id=%d  \"%s\"\n", i, g->items[i], idstr);
        }
        ss_toastf(iface, "group '%s': %d id(s)", name, g->count);
    } else {
        sh_printf("chkgrp: %d group(s):\n", g_group_count);
        for (int i = 0; i < g_group_count; i++)
            sh_printf("  '%s': %d id(s)\n", g_groups[i].name, g_groups[i].ids.count);
        if (g_group_count == 0) sh_printf("  (no groups defined)\n");
        ss_toastf(iface, "%d group(s) defined", g_group_count);
    }
}

/* Remove one group or all groups with *. Deleted groups no longer appear in listings. */
static void h_clrgrp(void *ctx, int argc, const char **argv)
{
    sh_iface *iface = (sh_iface *)ctx;
    const char *name = arg_at(argc, argv, 1);
    ss_sync();
    if (!name || !name[0]) { ic_toast(iface, "SnapStack", "usage: clrgrp <group>|*"); return; }
    if (strcmp(name, "*") == 0) {
        int n = g_group_count;
        group_remove_all();
        ss_toastf(iface, "clrgrp: deleted all %d group(s)", n);
        return;
    }
    if (!group_has(name)) { ss_toastf(iface, "clrgrp: no group named '%s'", name); return; }
    int had = group_remove(name);
    ss_toastf(iface, "clrgrp: deleted group '%s' (%d id(s))", name, had);
}

/* Command registration. */
typedef struct sh_subcommand { const char *name; sh_cmd_handler handler; } sh_subcommand;

static const sh_subcommand SNAPSTACK_COMMANDS[] = {
    { "psel",       h_psel },
    { "popsel",     h_popsel },
    { "phov",       h_phov },
    { "cstk",       h_cstk },
    { "pr",         h_pr },
    { "pg",         h_pg },
    { "pop2g",      h_pop2g },
    { "filtinh",    h_filtinh },
    { "filtcls",    h_filtcls },
    { "bss",        h_bss },
    { "bsi",        h_bsi },
    { "bsf",        h_bsf },
    { "bsb",        h_bsb },
    { "bse",        h_bse },
    { "accl",       h_accl },
    { "acctargets", h_acctargets },
    { "mkcmd",      h_mkcmd },
    { "bsin",       h_bsin },
    { "bscls",      h_bscls },
    { "bsincls",    h_bsincls },

    { "chkstk",     h_chkstk },
    { "chkgrp",     h_chkgrp },
    { "clrgrp",     h_clrgrp },
};
#define SNAPSTACK_COMMAND_COUNT ((int)(sizeof(SNAPSTACK_COMMANDS) / sizeof(SNAPSTACK_COMMANDS[0])))

void sh_register_snapstack_commands_backend(sh_iface *iface)
{
    ss_lock_init();   /* single-threaded here (backend bootstrap, before the frontend thread spins) */
    if (!iface || !iface->vtbl || !iface->vtbl->register_cmd) return;
    for (int i = 0; i < SNAPSTACK_COMMAND_COUNT; i++)
        iface->vtbl->register_cmd(iface, SNAPSTACK_COMMANDS[i].name, SNAPSTACK_COMMANDS[i].handler, iface);
}

/* Frontend stack access uses the same locked store as console handlers. */
void sh_snapstack_push_ids_backend(int index, const int *ids, int count)
{
    ss_sync_stacks();
    if (count > 0) stack_push(index, ids, count);
}

int sh_snapstack_clear_stack_backend(int index)
{
    ss_sync_stacks();
    ss_lock();
    ss_ids *s = stack_get(index);
    int had = s->count;
    ss_ids_clear(s);
    ss_unlock();
    return had;
}
