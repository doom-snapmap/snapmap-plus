/* snapstack.c -- see snapstack.h. Pure-C backend port of src/ui/snapstack.cpp's stores + 20 handlers.
 * Faithful to the Qt implementation's semantics + VERBATIM toast text (including the filtcls "had
 * inherit" OG-mislabel fix already present there); the JSON structural work (walk/create a dotted path,
 * dedup-merge a reference list) is delegated to json_patch.c instead of QJsonObject.
 *
 * SECOND ATTEMPT (2026-07-13): a first port (commit e7ee129, 2026-07-09) was hard-reset out after a
 * live-tested regression. Postmortem: it predated TWO fixes since established on the Qt/webview side
 * (see docs/backend-changes.md's deferred-apply writeup + the +0x298 normalize-timeline-inherit incident)
 * and carried both bugs itself: (1) every apply-op scheduled via the DEFERRED +0xd0 apply_edit -- the
 * exact split-commit-across-threads pattern that double-frees the decl-source block on the next map
 * teardown; (2) several persistent `static` scratch buffers (two 256KB JSON buffers x3 call sites, a 1MB
 * idstr_bufs[4096][256] table) -- the same BSS-footprint pattern that caused a controller-freelook
 * regression when tried on the timeline-inherit slot. This rewrite fixes both: every kind=0 apply-op now
 * tries the SYNCHRONOUS +0x290 apply_sync first (OG-faithful inline commit; see ic_apply below), falling
 * back to the deferred schedule only for an old backend without the slot; mkcmd (kind=1, a different
 * operation that never exhibited the crash) stays on the deferred path, matching Qt's own convention. All
 * scratch buffers are heap-allocated transiently per call (malloc/free), never static/BSS.
 *
 * Clean-room: ported from our own RE. Zero OG SnapHak bytes.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <float.h>
#include "snapstack.h"
#include "snaphak_iface.h"
#include "json_patch.h"
#include "commands.h"           /* sh_printf -- the snapstack_diag console report */
#include "backend_log.h"        /* backend_log -- the snapstack_diag persisted summary */
#include "mkcmd_template.inc"   /* SH_MKCMD_PREFAB_TEMPLATE_C -- the C-escaped embedded prefab template */

/* ============================================================ the growable int array ================
 * A minimal realloc-doubling int vector -- the same "small, linear, no external deps" philosophy
 * snaphak_iface.c's own cmd-map already uses. Backs both a single numbered stack and a named group. */
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
/* dedup-on-push: append only ids not already present; returns #pushed (psel's body). */
static int ss_ids_push_dedup(ss_ids *v, const int *ids, int n)
{
    int pushed = 0;
    for (int i = 0; i < n; i++) {
        if (ss_ids_contains(v, ids[i])) continue;
        if (!ss_ids_reserve(v, v->count + 1)) break;      /* OOM -> stop pushing further (degrade, no crash) */
        v->items[v->count++] = ids[i];
        pushed++;
    }
    return pushed;
}
static void ss_ids_clear(ss_ids *v) { v->count = 0; }      /* cstk: empty IN PLACE, capacity kept */
/* move_out: hand back a HEAP COPY of the ids + empty the source (FUN_180001d84). Caller frees *out. */
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
/* copy (non-consuming): hand back a HEAP COPY without touching the source (a named GROUP's reuse path). */
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

/* ============================================================ the STACK-OF-STACKS =================== */
#define SS_MAX_STACKS 256   /* generous vs. any real console usage; an absurd index arg clamps in, no grow needed */
static ss_ids g_stacks[SS_MAX_STACKS];

static int ss_clamp_index(int index)
{
    if (index < 0) return 0;
    if (index >= SS_MAX_STACKS) return SS_MAX_STACKS - 1;   /* clamp, never index OOB */
    return index;
}
static ss_ids *stack_get(int index) { return &g_stacks[ss_clamp_index(index)]; }
static int stack_push(int index, const int *ids, int n) { return ss_ids_push_dedup(stack_get(index), ids, n); }
static void stack_clear(int index) { ss_ids_clear(stack_get(index)); }
static int stack_move_out(int index, int **out) { return ss_ids_move_out(stack_get(index), out); }

/* ============================================================ the named GROUPS ======================= */
#define SS_GROUP_NAME_CAP 64
typedef struct ss_group { char name[SS_GROUP_NAME_CAP]; ss_ids ids; } ss_group;
static ss_group *g_groups = NULL;
static int        g_group_count = 0, g_group_cap = 0;

static ss_ids *group_get(const char *name)   /* lookup-or-insert (OG FUN_180003e9c) */
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
/* pop2g's move-into swap: the group BECOMES `ids` (replacing whatever it held). */
static void group_set(const char *name, const int *ids, int n)
{
    ss_ids *g = group_get(name);
    if (!g) return;
    ss_ids_free(g);
    if (n > 0 && ss_ids_reserve(g, n)) { memcpy(g->items, ids, (size_t)n * sizeof(int)); g->count = n; }
}
/* clrgrp: DELETE a group entirely -- free its id buffer, then shift the tail down (order preserved) so it
 * no longer appears in chkgrp. Returns the id-count it held (0 if the name wasn't a group). */
static int group_remove(const char *name)
{
    for (int i = 0; i < g_group_count; i++) {
        if (strcmp(g_groups[i].name, name) != 0) continue;
        int had = g_groups[i].ids.count;
        ss_ids_free(&g_groups[i].ids);                 /* free THIS group's buffer before the shift */
        for (int j = i; j < g_group_count - 1; j++)     /* shift the tail down (shallow-copies the ptrs) */
            g_groups[j] = g_groups[j + 1];
        g_group_count--;                                /* the vacated top slot's dup ptr is now unowned */
        return had;
    }
    return 0;
}
/* clrgrp *: DELETE every group. Frees all id buffers; the count drops to 0. */
static void group_remove_all(void)
{
    for (int i = 0; i < g_group_count; i++) ss_ids_free(&g_groups[i].ids);
    g_group_count = 0;
}

static int is_valid_group_name(const char *name)
{
    /* pop2g (0x2998 isalpha): first char must be a letter; empty fails. */
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

/* ============================================================ engine-touch helpers ================== */
/* Thin wrappers over the SAME sh_iface vtable slots Qt's snapstack.cpp calls -- this module runs
 * in-process with the backend that OWNS the interface object, but still goes through the vtable (not a
 * direct internal call) so the call shape matches the proven Qt implementation exactly. Every one
 * null-checks the slot -> a clean degrade on a partial/older interface. */

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
static void ic_set_classname(sh_iface *iface, int id, const char *v)
{
    if (iface && iface->vtbl && iface->vtbl->set_classname) iface->vtbl->set_classname(iface, id, v);
}
static void ic_set_inherit(sh_iface *iface, int id, const char *v)
{
    if (iface && iface->vtbl && iface->vtbl->set_inherit) iface->vtbl->set_inherit(iface, id, v);
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
/* +0x268 ATOMIC class+inherit set. 1=applied, 0=rejected (fatal combo -- caller MUST skip the rebuild),
 * -1=slot absent (old backend -> caller falls back to the legacy two-call sequence). */
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
#define SH_APPLY_JSON_CAP (256 * 1024)   /* mirrors Qt's own cap -- the full-entity JSON can be large */
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
/* +0x290 SYNCHRONOUS inline apply (OG-faithful): commit NOW on this (console-drain / UI) thread. Returns
 * the applied count (>=0), or -1 if the slot is absent (an older backend -> the caller falls back to the
 * deferred schedule). Mirrors src/ui/snapstack.cpp's iface_apply_sync exactly. */
static int ic_apply_sync(sh_iface *iface, const sh_apply_item *items, int n, const char *op)
{
    if (!iface || !iface->vtbl || !iface->vtbl->apply_sync) return -1;
    if (n <= 0) return 0;
    return iface->vtbl->apply_sync(iface, items, n, op);
}
/* Apply a batch the OG-faithful way: SYNCHRONOUS inline (+0x290) when the backend has it, else the
 * deferred +0xd0 schedule (older backend only). ALL kind=0 decl-edit ops (bss/bsi/bsf/bsb/bse/accl/
 * acctargets) go through this -- see the module doc comment for why (the deferred path double-frees the
 * decl-source block on the next map teardown; this is the exact bug the first port attempt carried).
 * mkcmd (kind=1, prefab paste) intentionally stays on ic_schedule_apply -- a different operation that
 * targets the editor paste slot, matching Qt's own convention. */
static int ic_apply(sh_iface *iface, const sh_apply_item *items, int n, const char *op)
{
    if (n <= 0) return 0;
    int applied = ic_apply_sync(iface, items, n, op);   /* +0x290 sync -> applied count (>=0), or -1 absent */
    if (applied >= 0) return 1;                          /* sync ran; the backend toasts the applied/total count */
    return ic_schedule_apply(iface, items, n, op);        /* old backend without +0x290 -> deferred fallback */
}

/* ============================================================ the 9 STORE-op handlers =============== */

/* psel [stack] (0x2108): read live selection -> push onto stack[N] (dedup) -> CLEAR selection -> toast. */
static void h_psel(void *ctx, int argc, const char **argv)
{
    sh_iface *iface = (sh_iface *)ctx;
    int index = parse_stack_index(arg_at(argc, argv, 1));
    enum { SEL_CAP = 4096 };
    int *sel = (int *)malloc((size_t)SEL_CAP * sizeof(int));
    int *valid = (int *)malloc((size_t)SEL_CAP * sizeof(int));
    if (!sel || !valid) { free(sel); free(valid); ic_toast(iface, "SnapStack", "psel: out of memory"); return; }
    int sel_n = ic_get_selection(iface, sel, SEL_CAP);
    int valid_n = 0, dropped = 0;
    for (int i = 0; i < sel_n; i++) {
        if (ic_is_valid_id(iface, sel[i])) valid[valid_n++] = sel[i]; else dropped++;
    }
    int pushed = stack_push(index, valid, valid_n);
    int now = stack_get(index)->count;
    free(sel);
    free(valid);
    ic_clear_selection(iface);
    char text[176];
    _snprintf_s(text, sizeof text, _TRUNCATE,
                "psel: selection=%d%s -> pushed %d onto stack %d (now %d).",
                sel_n, (dropped ? " (stale dropped)" : ""), pushed, index, now);
    ic_toast(iface, "SnapStack", text);
}

/* resolve a handler's operand arg (argv[1]) -> a HEAP-owned id array the caller must free. Reproduces OG
 * FUN_180001fa0: a GROUP NAME (letter-first, an existing group) -> COPY (preserved, reusable); else a
 * STACK INDEX -> MOVE OUT (consumed, left empty). */
static int resolve_operand_consume(int argc, const char **argv, int **out)
{
    const char *arg = arg_at(argc, argv, 1);
    if (arg && isalpha((unsigned char)arg[0]) && group_has(arg))
        return ss_ids_copy_out(group_get(arg), out);
    return stack_move_out(parse_stack_index(arg), out);
}

/* popsel [stack] (0x3a14): ADD each stored id back to the selection, then CONSUME the operand. No toast. */
static void h_popsel(void *ctx, int argc, const char **argv)
{
    sh_iface *iface = (sh_iface *)ctx;
    int *ids = NULL;
    int n = resolve_operand_consume(argc, argv, &ids);
    for (int i = 0; i < n; i++) ic_add_to_selection(iface, ids[i]);
    free(ids);
}

/* phov [stack] (0x20b4): push the HOVERED id onto stack[N], dedup. <0 -> push nothing. OG was silent; the
 * clone adds a confirm toast (naming the hovered entity + stack count) -- a hover-push is otherwise
 * invisible without a chkstk, and "nothing hovered" is a useful signal on a miss. */
static void h_phov(void *ctx, int argc, const char **argv)
{
    sh_iface *iface = (sh_iface *)ctx;
    int index = parse_stack_index(arg_at(argc, argv, 1));
    int hovered = ic_hovered_id(iface);
    if (hovered < 0) { ic_toast(iface, "SnapStack", "phov: nothing hovered (point at an entity in the 3D view)"); return; }
    int pushed = stack_push(index, &hovered, 1);
    int now = stack_get(index)->count;
    char idstr[256]; ic_id_string(iface, hovered, idstr, (int)sizeof idstr);
    char text[200];
    _snprintf_s(text, sizeof text, _TRUNCATE,
                "phov: %s id %d \"%s\" onto stack %d (now %d)",
                pushed ? "pushed" : "already on --", hovered, idstr, index, now);
    ic_toast(iface, "SnapStack", text);
}

/* cstk [stack] (0x2208): empty stack[N] in place. OG was silent; the clone adds a confirm toast (a
 * cleared stack is otherwise invisible without a chkstk). */
static void h_cstk(void *ctx, int argc, const char **argv)
{
    sh_iface *iface = (sh_iface *)ctx;
    int index = parse_stack_index(arg_at(argc, argv, 1));
    int had = stack_get(index)->count;
    stack_clear(index);
    char t[96];
    _snprintf_s(t, sizeof t, _TRUNCATE, "cstk: cleared stack %d (%d id(s) removed)", index, had);
    ic_toast(iface, "SnapStack", t);
}

/* pr <stack> <lo> <hi> (0x2c9c): push every VALID id in [lo..hi] -> stack[N], dedup; toast the span. */
static void h_pr(void *ctx, int argc, const char **argv)
{
    sh_iface *iface = (sh_iface *)ctx;
    int index = parse_stack_index(arg_at(argc, argv, 1));
    const char *lo_s = arg_at(argc, argv, 2);
    const char *hi_s = arg_at(argc, argv, 3);
    int lo = lo_s ? atoi(lo_s) : 0;
    int hi = hi_s ? atoi(hi_s) : 0;
    if (hi < lo) { int t = lo; lo = hi; hi = t; }
    int span = hi - lo;                                  /* DIRECT 0x2c9c: span, NOT inclusive count */
    enum { RANGE_CAP = 65536 };
    int *in_range = (int *)malloc((size_t)RANGE_CAP * sizeof(int));
    if (!in_range) { ic_toast(iface, "SnapStack", "pr: out of memory"); return; }
    int n = 0;
    for (int id = lo; id <= hi && n < RANGE_CAP; id++)
        if (ic_is_valid_id(iface, id)) in_range[n++] = id;
    int pushed = stack_push(index, in_range, n);
    free(in_range);
    char text[160];
    _snprintf_s(text, sizeof text, _TRUNCATE,
                "Pushed %d entities out of %d in range %d-%d", pushed, span, lo, hi);
    ic_toast(iface, "Pushed entities", text);
}

/* pg <stack> <group> (0x2b54): push the named group's ids -> stack[N], dedup; toast the count.
 * CONVENIENCE (same clone divergence as pop2g): a SINGLE letter-first arg is taken as the GROUP name with
 * stack 0 implied (`sh pg mygroup` == `sh pg 0 mygroup`). A numeric arg[1] means the stack (OG-faithful). */
static void h_pg(void *ctx, int argc, const char **argv)
{
    sh_iface *iface = (sh_iface *)ctx;
    const char *a1 = arg_at(argc, argv, 1);
    const char *a2 = arg_at(argc, argv, 2);
    int index;
    const char *name;
    if (a1 && isalpha((unsigned char)a1[0]) && !a2) { index = 0; name = a1; }
    else { index = parse_stack_index(a1); name = a2 ? a2 : ""; }
    int *ids = NULL;
    int n = ss_ids_copy_out(group_get(name), &ids);      /* a missing group auto-creates empty (push 0) */
    int pushed = stack_push(index, ids, n);
    free(ids);
    char text[256];
    _snprintf_s(text, sizeof text, _TRUNCATE, "Pushed %d entities from group %s onto stack %d",
                pushed, name, index);
    ic_toast(iface, "Pushed entities", text);
}

/* pop2g <stack> <group> (0x2998): MOVE stack[N] -> named group (swap). Name must start with a letter.
 * CONVENIENCE (clone divergence from OG's strict `<stack> <group>`): a SINGLE letter-first arg is taken as
 * the GROUP name with stack 0 implied (`sh pop2g mygroup` == `sh pop2g 0 mygroup`) -- consistent with how
 * resolve_operand_consume treats a letter-first operand as a group everywhere else. A numeric arg[1] still
 * means the stack, with arg[2] the group (OG-faithful). */
static void h_pop2g(void *ctx, int argc, const char **argv)
{
    sh_iface *iface = (sh_iface *)ctx;
    const char *a1 = arg_at(argc, argv, 1);
    const char *a2 = arg_at(argc, argv, 2);
    int index;
    const char *name;
    if (a1 && isalpha((unsigned char)a1[0]) && !a2) {   /* single letter-first arg -> group, stack 0 */
        index = 0;
        name = a1;
    } else {
        index = parse_stack_index(a1);
        name = a2 ? a2 : "";
    }
    if (!is_valid_group_name(name)) {
        char text[256];
        _snprintf_s(text, sizeof text, _TRUNCATE,
                    "%s is not a valid group name because it needs to start with a letter", name);
        ic_toast(iface, "Invalid groupname", text);
        return;
    }
    int *ids = NULL;
    int n = stack_move_out(index, &ids);
    group_set(name, ids, n);
    free(ids);
    char text[200];
    _snprintf_s(text, sizeof text, _TRUNCATE,
                "pop2g: moved %d id(s) from stack %d into group '%s' (stack %d now empty)",
                n, index, name, index);
    ic_toast(iface, "SnapStack", text);
}

/* filtinh / filtcls shared body (0x3c70 / 0x3c78): KEEP only stack[N] ids whose inherit/classname ==
 * match; re-push the survivors; toast the count. filtcls toast is labeled "had class" (the OG mislabel
 * on filtcls -- reusing filtinh's "had inherit" string -- is fixed here, matching Qt). */
static void do_filt(sh_iface *iface, int index, const char *match, int by_class)
{
    int *cur = NULL;
    int n = stack_move_out(index, &cur);
    int *kept = (n > 0) ? (int *)malloc((size_t)n * sizeof(int)) : NULL;
    int kn = 0;
    for (int i = 0; i < n; i++) {
        char field[256];
        if (by_class) ic_classname(iface, cur[i], field, (int)sizeof field);
        else          ic_inherit(iface, cur[i], field, (int)sizeof field);
        if (strcmp(field, match) == 0) kept[kn++] = cur[i];
    }
    stack_push(index, kept, kn);
    free(cur);
    free(kept);
    char text[256];
    _snprintf_s(text, sizeof text, _TRUNCATE, "%d entities on stack %d had %s %s", kn, index,
                by_class ? "class" : "inherit", match);
    ic_toast(iface, "Filter", text);
}
static void h_filtinh(void *ctx, int argc, const char **argv)
{
    sh_iface *iface = (sh_iface *)ctx;
    int index = parse_stack_index(arg_at(argc, argv, 1));
    const char *match = arg_at(argc, argv, 2);
    do_filt(iface, index, match ? match : "", 0);
}
static void h_filtcls(void *ctx, int argc, const char **argv)
{
    sh_iface *iface = (sh_iface *)ctx;
    int index = parse_stack_index(arg_at(argc, argv, 1));
    const char *match = arg_at(argc, argv, 2);
    do_filt(iface, index, match ? match : "", 1);
}

/* ============================================================ the 8-pass apply chain ================ */

/* C atoi / atof (the leading-token forms the leaf encoders need). */
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

/* renderEngineFloat: the ENGINE-FORMAT float token -- the shortest round-trip decimal, keeping a
 * trailing ".0" on whole floats and switching to scientific notation at exp<-4 or exp>=16 (matches Qt's
 * own render_engine_float byte-for-byte; ported to fixed buffers, no std::string). */
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

/* the per-op leaf-value ENCODING: bss -> a JSON string literal; bsi -> a bare int; bsf ->
 * renderEngineFloat(fround(atof)); bsb -> "true"/"false". Returns the RAW JSON leaf TOKEN (spliced
 * verbatim by json_patch.c -- never re-escaped). */
static int encode_bulkset_leaf_json(const char *op, const char *value, char *out, int outcap)
{
    if (strcmp(op, "bsi") == 0) {
        _snprintf_s(out, (size_t)outcap, _TRUNCATE, "%ld", sh_c_atoi(value));
        return 1;
    }
    if (strcmp(op, "bsf") == 0) {
        float f32 = (float)sh_c_atof(value);            /* Math.fround: narrow to float32 */
        render_engine_float((double)f32, out, outcap);
        return 1;
    }
    if (strcmp(op, "bsb") == 0) {
        _snprintf_s(out, (size_t)outcap, _TRUNCATE, "%s", (strcmp(value, "true") == 0) ? "true" : "false");
        return 1;
    }
    return sh_json_quote_string(value, out, outcap);      /* bss / default: a JSON string literal */
}

/* --- bss/bsi/bsf/bsb shared body: per id serialize -> patch the typed leaf -> collect; schedule the
 * batch. argv: [0]=op [1]=stack [2]=propPath [3]=value. */
static void do_bulkset(sh_iface *iface, const char *op, int argc, const char **argv)
{
    const char *prop = arg_at(argc, argv, 2);
    const char *val  = arg_at(argc, argv, 3);
    if (!prop || !val) { char t[64]; _snprintf_s(t, sizeof t, _TRUNCATE, "usage: %s <stack> <path> <value>", op); ic_toast(iface, "SnapStack", t); return; }
    int *ids = NULL;
    int n = resolve_operand_consume(argc, argv, &ids);      /* CONSUME the stack (OG drains on use) */
    if (n <= 0) { free(ids); ic_toast(iface, "SnapStack", "no entities on the stack"); return; }

    char leaf[64];
    if (!encode_bulkset_leaf_json(op, val, leaf, (int)sizeof leaf)) { free(ids); ic_toast(iface, "SnapStack", "leaf encode failed"); return; }

    /* full/scratch are HEAP-ALLOCATED TRANSIENTLY (malloc per call, freed before return) -- never a
     * persistent static/BSS slot (see the module doc comment: this is the exact footprint pattern that
     * caused a controller-freelook regression on the +0x298 timeline-inherit slot). Reused across the id
     * loop within this one call; each item's PATCHED text is then heap-copied to exactly its own length.
     * apply_sync/apply_edit deep-copy the text into their own storage, so these heap copies only need to
     * outlive the apply call. */
    char *full = (char *)malloc(SH_APPLY_JSON_CAP);
    char *scratch = (char *)malloc(SH_APPLY_JSON_CAP + 1024);
    if (!full || !scratch) {
        free(full); free(scratch); free(ids);
        ic_toast(iface, "SnapStack", "out of memory"); return;
    }
    sh_apply_item items[512];
    char *owned[512] = {0};
    int m = 0;
    for (int i = 0; i < n && m < 512; i++) {
        if (!ic_serialize_entity(iface, ids[i], full, SH_APPLY_JSON_CAP)) continue;
        if (!sh_json_patch_set_leaf(full, prop, leaf, scratch, SH_APPLY_JSON_CAP + 1024)) continue;
        size_t plen = strlen(scratch);
        char *copy = (char *)malloc(plen + 1);
        if (!copy) continue;
        memcpy(copy, scratch, plen + 1);
        owned[m] = copy;
        items[m].kind = 0; items[m].id = ids[i]; items[m].text = copy;
        m++;
    }
    free(full);
    free(scratch);
    free(ids);
    if (m == 0) { ic_toast(iface, "SnapStack", "serialize/patch produced no apply"); return; }
    int ok = ic_apply(iface, items, m, op);   /* +0x290 SYNCHRONOUS inline (OG-faithful); deferred fallback */
    for (int i = 0; i < m; i++) free(owned[i]);
    if (!ok) {
        char t[96]; _snprintf_s(t, sizeof t, _TRUNCATE, "%s: apply failed (editor down?)", op);
        ic_toast(iface, "SnapStack", t);
    }
}
static void h_bss(void *ctx, int argc, const char **argv) { do_bulkset((sh_iface *)ctx, "bss", argc, argv); }
static void h_bsi(void *ctx, int argc, const char **argv) { do_bulkset((sh_iface *)ctx, "bsi", argc, argv); }
static void h_bsf(void *ctx, int argc, const char **argv) { do_bulkset((sh_iface *)ctx, "bsf", argc, argv); }

/* bsb: bulk-set BOOL. A serialize/patch mismatch is a real signal (a property/value that didn't
 * round-trip) -- surfaced via a clean toast (the OG's own leftover debug MessageBoxA is NOT reproduced;
 * matches Qt's already-fixed behavior, not the historical OG bug). */
static void h_bsb(void *ctx, int argc, const char **argv)
{
    sh_iface *iface = (sh_iface *)ctx;
    const char *prop = arg_at(argc, argv, 2);
    const char *val  = arg_at(argc, argv, 3);
    if (!prop || !val) { ic_toast(iface, "SnapStack", "usage: bsb <stack> <path> true|false"); return; }
    int *ids = NULL;
    int n = resolve_operand_consume(argc, argv, &ids);
    if (n <= 0) { free(ids); ic_toast(iface, "SnapStack", "no entities on the stack"); return; }

    char leaf[8];
    encode_bulkset_leaf_json("bsb", val, leaf, (int)sizeof leaf);

    char *full = (char *)malloc(SH_APPLY_JSON_CAP);           /* heap-transient (see module doc comment) */
    char *scratch = (char *)malloc(SH_APPLY_JSON_CAP + 1024);
    if (!full || !scratch) {
        free(full); free(scratch); free(ids);
        ic_toast(iface, "SnapStack", "out of memory"); return;
    }
    sh_apply_item items[512];
    char *owned[512] = {0};
    int m = 0, mismatch = 0;
    for (int i = 0; i < n && m < 512; i++) {
        if (!ic_serialize_entity(iface, ids[i], full, SH_APPLY_JSON_CAP)) { mismatch = 1; continue; }
        if (!sh_json_patch_set_leaf(full, prop, leaf, scratch, SH_APPLY_JSON_CAP + 1024)) { mismatch = 1; continue; }
        size_t plen = strlen(scratch);
        char *copy = (char *)malloc(plen + 1);
        if (!copy) { mismatch = 1; continue; }
        memcpy(copy, scratch, plen + 1);
        owned[m] = copy;
        items[m].kind = 0; items[m].id = ids[i]; items[m].text = copy;
        m++;
    }
    free(full);
    free(scratch);
    free(ids);
    if (mismatch) ic_toast(iface, "SnapStack", "bsb: some entities skipped (property/value re-resolve mismatch)");
    if (m > 0) {
        int ok = ic_apply(iface, items, m, "bsb");   /* +0x290 SYNCHRONOUS inline; deferred fallback */
        for (int i = 0; i < m; i++) free(owned[i]);
        if (!ok) ic_toast(iface, "SnapStack", "bsb: apply failed (editor down?)");
    } else if (!mismatch) {
        ic_toast(iface, "SnapStack", "bsb: serialize/patch produced no apply");
    }
}

/* bse (0x2720): pop LAST id -> its id-STRING; for EACH remaining id set state.edit.<userPath> = that
 * id-string (delegates to the same scalar leaf-set as bss). >=2 ids, STACK-ONLY (no group operand). */
static void h_bse(void *ctx, int argc, const char **argv)
{
    sh_iface *iface = (sh_iface *)ctx;
    const char *prop = arg_at(argc, argv, 2);
    if (!prop) { ic_toast(iface, "SnapStack", "usage: bse <stack> <path>"); return; }
    int index = parse_stack_index(arg_at(argc, argv, 1));
    if (stack_get(index)->count < 2) { ic_toast(iface, "SnapStack", "bse needs >= 2 ids on the stack"); return; }
    int *ids = NULL;
    int n = stack_move_out(index, &ids);         /* CONSUME the stack (OG-faithful) */
    int popped = ids[n - 1];
    int remaining_n = n - 1;

    char poppedStr[256]; ic_id_string(iface, popped, poppedStr, (int)sizeof poppedStr);
    char leaf[300]; sh_json_quote_string(poppedStr, leaf, (int)sizeof leaf);

    char *full = (char *)malloc(SH_APPLY_JSON_CAP);           /* heap-transient (see module doc comment) */
    char *scratch = (char *)malloc(SH_APPLY_JSON_CAP + 1024);
    if (!full || !scratch) {
        free(full); free(scratch); free(ids);
        ic_toast(iface, "SnapStack", "out of memory"); return;
    }
    sh_apply_item items[512];
    char *owned[512] = {0};
    int m = 0;
    for (int i = 0; i < remaining_n && m < 512; i++) {
        if (!ic_serialize_entity(iface, ids[i], full, SH_APPLY_JSON_CAP)) continue;
        if (!sh_json_patch_set_leaf(full, prop, leaf, scratch, SH_APPLY_JSON_CAP + 1024)) continue;
        size_t plen = strlen(scratch);
        char *copy = (char *)malloc(plen + 1);
        if (!copy) continue;
        memcpy(copy, scratch, plen + 1);
        owned[m] = copy;
        items[m].kind = 0; items[m].id = ids[i]; items[m].text = copy;
        m++;
    }
    free(full);
    free(scratch);
    free(ids);
    if (m == 0) { ic_toast(iface, "SnapStack", "bse: produced no apply"); return; }
    int ok = ic_apply(iface, items, m, "bse");   /* +0x290 SYNCHRONOUS inline (OG-faithful); deferred fallback */
    for (int i = 0; i < m; i++) free(owned[i]);
    if (!ok) ic_toast(iface, "SnapStack", "bse: apply failed (editor down?)");
}

/* accl/acctargets shared body (0x2498 / 0x228c): pop LAST id (=RECEIVER); build a num/item[] LIST of ALL
 * remaining ids' id-strings at state.edit.<path>; apply on the POPPED id. acctargets HARDCODES path
 * "targets". >=2 ids, STACK-ONLY. Drops stale/deleted ids from the list (exceeds OG -- see Qt's own
 * comment on why: a dangling id-string is never a valid target). */
static void do_acc(sh_iface *iface, const char *op, int argc, const char **argv, int hardcoded_targets)
{
    const char *userPath = arg_at(argc, argv, 2);
    if (!hardcoded_targets && !userPath) { ic_toast(iface, "SnapStack", "usage: accl <stack> <path>"); return; }
    int index = parse_stack_index(arg_at(argc, argv, 1));
    if (stack_get(index)->count < 2) {
        char t[64]; _snprintf_s(t, sizeof t, _TRUNCATE, "%s needs >= 2 ids", op);
        ic_toast(iface, "SnapStack", t); return;
    }
    int *ids = NULL;
    int n = stack_move_out(index, &ids);          /* CONSUME the stack (OG-faithful) */
    int popped = ids[n - 1];
    int remaining_n = n - 1;
    const char *path = hardcoded_targets ? "targets" : userPath;

    /* heap-transient, sized to remaining_n (never a persistent 1MB static table -- see module doc comment). */
    int cap = remaining_n > 0 ? remaining_n : 1;
    const char **idstr_ptrs = (const char **)malloc((size_t)cap * sizeof(char *));
    char *idstr_bufs = (char *)malloc((size_t)cap * 256);
    if (!idstr_ptrs || !idstr_bufs) {
        free(idstr_ptrs); free(idstr_bufs); free(ids);
        ic_toast(iface, "SnapStack", "out of memory"); return;
    }
    int nvalid = 0, skipped = 0;
    for (int i = 0; i < remaining_n; i++) {
        if (!ic_is_valid_id(iface, ids[i])) { skipped++; continue; }   /* stale/deleted -> not a target */
        char *slot = idstr_bufs + (size_t)nvalid * 256;
        ic_id_string(iface, ids[i], slot, 256);
        idstr_ptrs[nvalid] = slot;
        nvalid++;
    }
    free(ids);
    if (nvalid == 0) {
        free(idstr_ptrs); free(idstr_bufs);
        char t[112];
        _snprintf_s(t, sizeof t, _TRUNCATE,
            "%s: no VALID targets on the stack (all %d were stale/deleted -- clear the stack with cstk)",
            op, remaining_n);
        ic_toast(iface, "SnapStack", t); return;
    }
    if (skipped > 0) {
        char t[112];
        _snprintf_s(t, sizeof t, _TRUNCATE, "%s: skipped %d stale/deleted id(s); listed %d valid target(s)",
                    op, skipped, nvalid);
        ic_toast(iface, "SnapStack", t);
    }

    char *full = (char *)malloc(SH_APPLY_JSON_CAP);
    char *patched = (char *)malloc(SH_APPLY_JSON_CAP + 4096);
    if (!full || !patched) {
        free(idstr_ptrs); free(idstr_bufs); free(full); free(patched);
        ic_toast(iface, "SnapStack", "out of memory"); return;
    }
    if (!ic_serialize_entity(iface, popped, full, SH_APPLY_JSON_CAP)) {
        free(idstr_ptrs); free(idstr_bufs); free(full); free(patched);
        ic_toast(iface, "SnapStack", "accl: receiver serialize failed"); return;
    }
    if (!sh_json_patch_upsert_reflist(full, path, idstr_ptrs, nvalid, patched, SH_APPLY_JSON_CAP + 4096)) {
        free(idstr_ptrs); free(idstr_bufs); free(full); free(patched);
        ic_toast(iface, "SnapStack", "accl: list patch failed"); return;
    }
    free(idstr_ptrs);
    free(idstr_bufs);
    free(full);
    sh_apply_item it; it.kind = 0; it.id = popped; it.text = patched;
    int ok = ic_apply(iface, &it, 1, op);   /* +0x290 SYNCHRONOUS inline (OG-faithful); deferred fallback */
    free(patched);
    if (!ok) { ic_toast(iface, "SnapStack", "accl: apply failed (editor down?)"); return; }

    /* success toast: how many targets/refs + WHICH receiver got them. The backend's generic "applied N/N
     * (engine round-trip)" toast is SUPPRESSED for accl/acctargets in ae_toast_result (apply_engine.c), so
     * without this the op succeeds SILENTLY -- matches Qt's do_acc, which emits this receiver toast instead. */
    {
        char rcv[256]; ic_id_string(iface, popped, rcv, (int)sizeof rcv);
        char t[160];
        _snprintf_s(t, sizeof t, _TRUNCATE, "%s: %d %s applied to \"%s\"",
                    op, nvalid, hardcoded_targets ? "target(s)" : "ref(s)", rcv);
        ic_toast(iface, "SnapStack", t);
    }
}
static void h_accl(void *ctx, int argc, const char **argv)       { do_acc((sh_iface *)ctx, "accl", argc, argv, 0); }
static void h_acctargets(void *ctx, int argc, const char **argv) { do_acc((sh_iface *)ctx, "acctargets", argc, argv, 1); }

/* mkcmd (0x3744): synthesize an idSnapEntityPrefab command-entity into the paste-staging slot. Per id:
 * "ai_ScriptCmdEnt " + template.replace('$', idString), joined with ';' (trailing ';'). Spliced into the
 * byte-exact prefab template at its placeholder; scheduled as kind=1 (mkcmd prefab paste). */
static void h_mkcmd(void *ctx, int argc, const char **argv)
{
    sh_iface *iface = (sh_iface *)ctx;
    int *ids = NULL;
    int n = resolve_operand_consume(argc, argv, &ids);
    if (n <= 0) { free(ids); ic_toast(iface, "SnapStack", "mkcmd: no entities on the stack"); return; }
    const char *tmpl_arg = arg_at(argc, argv, 2);
    const char *tmpl = (tmpl_arg && tmpl_arg[0]) ? tmpl_arg : "$";

    char *commandText = (char *)malloc(SH_APPLY_JSON_CAP);   /* heap-transient (see module doc comment) */
    if (!commandText) { free(ids); ic_toast(iface, "SnapStack", "out of memory"); return; }
    size_t clen = 0;
    commandText[0] = '\0';
    for (int i = 0; i < n; i++) {
        char idStr[256]; ic_id_string(iface, ids[i], idStr, (int)sizeof idStr);
        char part[512]; size_t plen = 0;
        int wrote = _snprintf_s(part, sizeof part, _TRUNCATE, "ai_ScriptCmdEnt ");
        plen = (wrote > 0) ? (size_t)wrote : 0;
        for (const char *c = tmpl; *c && plen + 1 < sizeof part; c++) {
            if (*c == '$') { size_t idlen = strlen(idStr); if (plen + idlen + 1 >= sizeof part) break; memcpy(part + plen, idStr, idlen); plen += idlen; }
            else part[plen++] = *c;
        }
        part[plen] = '\0';
        if (clen + plen + 2 >= SH_APPLY_JSON_CAP) break;
        memcpy(commandText + clen, part, plen); clen += plen;
        commandText[clen++] = ';';   /* OG appends ';' after EACH part -> trailing ';' */
    }
    commandText[clen] = '\0';
    free(ids);

    char *prefab = (char *)malloc(SH_APPLY_JSON_CAP + 4096);   /* heap-transient (see module doc comment) */
    if (!prefab) { free(commandText); ic_toast(iface, "SnapStack", "out of memory"); return; }
    const char *ph = "__SNAPHAK_MKCMD_COMMANDTEXT__";
    const char *tmplsrc = SH_MKCMD_PREFAB_TEMPLATE_C;
    const char *pos = strstr(tmplsrc, ph);
    if (!pos) { free(commandText); free(prefab); ic_toast(iface, "SnapStack", "mkcmd: template missing placeholder"); return; }
    size_t pre = (size_t)(pos - tmplsrc);
    size_t phlen = strlen(ph);
    size_t total = pre + clen + strlen(tmplsrc + pre + phlen);
    if (total + 1 > (size_t)(SH_APPLY_JSON_CAP + 4096)) {
        free(commandText); free(prefab);
        ic_toast(iface, "SnapStack", "mkcmd: commandText too large"); return;
    }
    memcpy(prefab, tmplsrc, pre);
    memcpy(prefab + pre, commandText, clen);
    _snprintf_s(prefab + pre + clen, (size_t)(SH_APPLY_JSON_CAP + 4096) - pre - clen, _TRUNCATE, "%s", tmplsrc + pre + phlen);
    free(commandText);

    /* mkcmd (kind=1, prefab paste) intentionally stays on the DEFERRED path -- it targets the editor paste
     * slot, a different operation from the kind=0 decl-edits, and never exhibited the double-free crash
     * (matches Qt's own snapstack.cpp convention; see the module doc comment for the kind=0 rationale). */
    sh_apply_item it; it.kind = 1; it.id = 0; it.text = prefab;
    int ok = ic_schedule_apply(iface, &it, 1, "mkcmd");
    free(prefab);
    if (!ok) ic_toast(iface, "SnapStack", "mkcmd: schedule failed (editor down?)");
}

/* ============================================================ class/inherit-CHANGE handlers ========= */

/* FUN_180001244(id, cls): set className (+0x78), then re-emit decl-source (+0x30 -> +0x40). ALWAYS
 * rebuilds; re-asserts the classname after (the rebuild's re-parse is last-wins from the appended old
 * source, so the explicit value must win over it). */
static void do_set_classname_one(sh_iface *iface, int id, const char *cls)
{
    ic_set_classname(iface, id, cls);
    char *r = (char *)malloc(64 * 1024);      /* heap-transient (see module doc comment) */
    if (!r) return;
    ic_declsource_text(iface, id, r, 64 * 1024);
    ic_rebuild_declsource(iface, id, r);
    free(r);
    ic_set_classname(iface, id, cls);      /* re-assert -- wins over the rebuild's revert */
}
/* FUN_1800012dc(id, inh, rebuild): set inherit (+0x80); if rebuild, re-emit decl-source + re-assert. */
static void do_set_inherit_one(sh_iface *iface, int id, const char *inh, int rebuild)
{
    ic_set_inherit(iface, id, inh);
    if (rebuild) {
        char *r = (char *)malloc(64 * 1024);  /* heap-transient (see module doc comment) */
        if (!r) return;
        ic_declsource_text(iface, id, r, 64 * 1024);
        ic_rebuild_declsource(iface, id, r);
        free(r);
        ic_set_inherit(iface, id, inh);    /* re-assert -- wins over the rebuild's revert */
    }
}

/* bscls <stack> <newClassName>: per stack id, set className + rebuild (the light path). */
static void h_bscls(void *ctx, int argc, const char **argv)
{
    sh_iface *iface = (sh_iface *)ctx;
    const char *cls = arg_at(argc, argv, 2);
    if (!cls || !cls[0]) { ic_toast(iface, "SnapStack", "usage: bscls <stack> <className>"); return; }
    int *ids = NULL;
    int n = resolve_operand_consume(argc, argv, &ids);
    if (n <= 0) { free(ids); ic_toast(iface, "SnapStack", "no entities on the stack"); return; }
    for (int i = 0; i < n; i++) do_set_classname_one(iface, ids[i], cls);
    free(ids);
    char text[160]; _snprintf_s(text, sizeof text, _TRUNCATE, "bscls: set className=%s on %d entities", cls, n);
    ic_toast(iface, "SnapStack", text);
}

/* bsin <stack> <newInherit>: per stack id, set inherit + rebuild (the heavy path). */
static void h_bsin(void *ctx, int argc, const char **argv)
{
    sh_iface *iface = (sh_iface *)ctx;
    const char *inh = arg_at(argc, argv, 2);
    if (!inh || !inh[0]) { ic_toast(iface, "SnapStack", "usage: bsin <stack> <inherit>"); return; }
    int *ids = NULL;
    int n = resolve_operand_consume(argc, argv, &ids);
    if (n <= 0) { free(ids); ic_toast(iface, "SnapStack", "no entities on the stack"); return; }
    for (int i = 0; i < n; i++) do_set_inherit_one(iface, ids[i], inh, 1);
    free(ids);
    char text[160]; _snprintf_s(text, sizeof text, _TRUNCATE, "bsin: set inherit=%s on %d entities", inh, n);
    ic_toast(iface, "SnapStack", text);
}

/* bsincls <stack> <newInherit> <newClassName>: atomic FINAL-pair set (+0x268) -> ONE rebuild -> re-assert;
 * falls back to the legacy two-call sequence if the atomic slot is absent (old backend). */
static void h_bsincls(void *ctx, int argc, const char **argv)
{
    sh_iface *iface = (sh_iface *)ctx;
    const char *inh = arg_at(argc, argv, 2);
    const char *cls = arg_at(argc, argv, 3);
    if (!inh || !inh[0] || !cls || !cls[0]) { ic_toast(iface, "SnapStack", "usage: bsincls <stack> <inherit> <className>"); return; }
    int *ids = NULL;
    int n = resolve_operand_consume(argc, argv, &ids);
    if (n <= 0) { free(ids); ic_toast(iface, "SnapStack", "no entities on the stack"); return; }
    for (int i = 0; i < n; i++) {
        int r = ic_apply_class_inherit(iface, ids[i], cls, inh);   /* +0x268 atomic (pass 1) */
        if (r == 1) {
            char *rsrc = (char *)malloc(64 * 1024);   /* heap-transient (see module doc comment) */
            if (rsrc) {
                ic_declsource_text(iface, ids[i], rsrc, 64 * 1024);
                ic_rebuild_declsource(iface, ids[i], rsrc);
                free(rsrc);
            }
            ic_apply_class_inherit(iface, ids[i], cls, inh);       /* re-assert (pass 2) */
        } else if (r == -1) {
            do_set_inherit_one(iface, ids[i], inh, 0);
            do_set_classname_one(iface, ids[i], cls);
        }
        /* r==0: the FINAL pair is a fatal combo -> leave unchanged, skip the rebuild (matches Qt). */
    }
    free(ids);
    char text[200];
    _snprintf_s(text, sizeof text, _TRUNCATE, "bsincls: set inherit=%s className=%s on %d entities", inh, cls, n);
    ic_toast(iface, "SnapStack", text);
}

/* ============================================================ store inspection / management =========
 * chkstk / chkgrp / clrgrp -- NEW backend-exclusive SnapStack+ commands (NOT part of OG's 20). They read
 * and manage the SAME file-static stores (g_stacks / g_groups) every op above mutates, so they report the
 * true live state under the webview build (where all SnapStack ops run these backend handlers). Under a Qt
 * build the ops run Qt's OWN in-process stores (snapstack.cpp), so these would read the backend's separate
 * (empty) copy -- a known store-duplication limitation the eventual Qt-retirement (Phase 2) resolves. All
 * output goes to the console (sh_printf) with a summary toast, mirroring snapstack_diag. */

/* chkstk [N]: N given -> list stack N's ids + count; omitted -> summarize every non-empty stack. */
static void h_chkstk(void *ctx, int argc, const char **argv)
{
    sh_iface *iface = (sh_iface *)ctx;
    const char *arg = arg_at(argc, argv, 1);
    if (arg && arg[0]) {
        int index = ss_clamp_index(parse_stack_index(arg));
        ss_ids *s = stack_get(index);
        sh_printf("chkstk: stack %d holds %d id(s):\n", index, s->count);
        for (int i = 0; i < s->count; i++) {
            char idstr[256]; ic_id_string(iface, s->items[i], idstr, (int)sizeof idstr);
            sh_printf("  [%d] id=%d  \"%s\"\n", i, s->items[i], idstr);
        }
        char t[96]; _snprintf_s(t, sizeof t, _TRUNCATE, "stack %d: %d id(s)", index, s->count);
        ic_toast(iface, "SnapStack", t);
    } else {
        int nonempty = 0, total = 0;
        sh_printf("chkstk: non-empty stacks:\n");
        for (int i = 0; i < SS_MAX_STACKS; i++) {
            if (g_stacks[i].count == 0) continue;
            sh_printf("  stack %d: %d id(s)\n", i, g_stacks[i].count);
            nonempty++; total += g_stacks[i].count;
        }
        if (nonempty == 0) sh_printf("  (all %d stacks empty)\n", SS_MAX_STACKS);
        char t[112]; _snprintf_s(t, sizeof t, _TRUNCATE,
            "%d non-empty stack(s), %d id(s) total", nonempty, total);
        ic_toast(iface, "SnapStack", t);
    }
}

/* chkgrp [name]: name given -> list that group's ids + count; omitted -> list all group names + counts. */
static void h_chkgrp(void *ctx, int argc, const char **argv)
{
    sh_iface *iface = (sh_iface *)ctx;
    const char *name = arg_at(argc, argv, 1);
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
        char t[112]; _snprintf_s(t, sizeof t, _TRUNCATE, "group '%s': %d id(s)", name, g->count);
        ic_toast(iface, "SnapStack", t);
    } else {
        sh_printf("chkgrp: %d group(s):\n", g_group_count);
        for (int i = 0; i < g_group_count; i++)
            sh_printf("  '%s': %d id(s)\n", g_groups[i].name, g_groups[i].ids.count);
        if (g_group_count == 0) sh_printf("  (no groups defined)\n");
        char t[64]; _snprintf_s(t, sizeof t, _TRUNCATE, "%d group(s) defined", g_group_count);
        ic_toast(iface, "SnapStack", t);
    }
}

/* clrgrp <name>|*: DELETE a named group entirely (its entry is removed, so it no longer shows in chkgrp
 * and `popsel <name>` re-creates it empty); `*` deletes every group. Fixes "groups persist forever +
 * popsel keeps re-selecting them + chkgrp clutters with stale groups". */
static void h_clrgrp(void *ctx, int argc, const char **argv)
{
    sh_iface *iface = (sh_iface *)ctx;
    const char *name = arg_at(argc, argv, 1);
    if (!name || !name[0]) { ic_toast(iface, "SnapStack", "usage: clrgrp <group>|*"); return; }
    if (strcmp(name, "*") == 0) {
        int n = g_group_count;
        group_remove_all();
        char t[96]; _snprintf_s(t, sizeof t, _TRUNCATE, "clrgrp: deleted all %d group(s)", n);
        ic_toast(iface, "SnapStack", t);
        return;
    }
    if (!group_has(name)) {
        char t[128]; _snprintf_s(t, sizeof t, _TRUNCATE, "clrgrp: no group named '%s'", name);
        ic_toast(iface, "SnapStack", t); return;
    }
    int had = group_remove(name);
    char t[128]; _snprintf_s(t, sizeof t, _TRUNCATE, "clrgrp: deleted group '%s' (%d id(s))", name, had);
    ic_toast(iface, "SnapStack", t);
}

/* ============================================================ diagnostics =========================== */

/* Resolve which loaded DLL owns a given code address (any function pointer). NULL / empty on failure --
 * never a crash, this only ever runs from an explicit console command. */
static void diag_owning_module(const void *code_addr, char *out, int outcap)
{
    out[0] = '\0';
    HMODULE hmod = NULL;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)code_addr, &hmod) || !hmod)
        return;
    char path[MAX_PATH] = {0};
    if (!GetModuleFileNameA(hmod, path, MAX_PATH)) return;
    const char *base = strrchr(path, '\\');
    base = base ? base + 1 : path;
    _snprintf_s(out, (size_t)outcap, _TRUNCATE, "%s", base);
}

/* forward-declared: registered as a 21st entry in SNAPSTACK_COMMANDS below, but its BODY (further down)
 * needs that same table to iterate -- the array only needs this prototype to take its address. */
static void h_snapstack_diag(void *ctx, int argc, const char **argv);

/* ============================================================ the registrar ========================= */
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
    /* NEW backend-exclusive SnapStack+ store-management commands (NOT among OG's 20; Qt never defines these
     * names, so no overwrite race -- they always run this backend's copy against the live backend stores). */
    { "chkstk",     h_chkstk },
    { "chkgrp",     h_chkgrp },
    { "clrgrp",     h_clrgrp },
    /* diagnostic-only: not one of the 20 OG-faithful subcommands, backend-exclusive (Qt never defines
     * this name, so there's no overwrite race for it -- it always reports on whatever's ACTUALLY live). */
    { "snapstack_diag", h_snapstack_diag },
};
#define SNAPSTACK_COMMAND_COUNT ((int)(sizeof(SNAPSTACK_COMMANDS) / sizeof(SNAPSTACK_COMMANDS[0])))

/* sh snapstack_diag: for each registered subcommand name (including itself), report which ACTUAL DLL
 * module currently owns the handler in the shared cmd-map (resolved via the handler's own code address
 * -- correct regardless of which frontend, if any, loaded, and regardless of whether it registered its
 * own copy over the backend's). Answers "is Qt's or the webview backend's copy of `sh psel` active right
 * now?" without guessing -- console output (sh_printf), a persisted one-line summary (backend_log), and
 * a toast (visible without opening the console history). */
static void h_snapstack_diag(void *ctx, int argc, const char **argv)
{
    (void)argc; (void)argv;
    sh_iface *iface = (sh_iface *)ctx;
    char self_mod[MAX_PATH]; diag_owning_module((const void *)h_snapstack_diag, self_mod, MAX_PATH);
    sh_printf("snapstack_diag: this backend is '%s'; current owner of each SnapStack subcommand:\n",
              self_mod[0] ? self_mod : "(unresolved)");
    int backend_owned = 0, other_owned = 0, missing = 0;
    for (int i = 0; i < SNAPSTACK_COMMAND_COUNT; i++) {
        const char *name = SNAPSTACK_COMMANDS[i].name;
        sh_cmd_handler handler = NULL; void *hctx = NULL;
        if (!sh_iface_lookup_cmd(iface, name, &handler, &hctx) || !handler) {
            sh_printf("  %-16s -> NOT REGISTERED\n", name);
            missing++;
            continue;
        }
        char owner[MAX_PATH]; diag_owning_module((const void *)handler, owner, MAX_PATH);
        int is_backend = owner[0] && self_mod[0] && _stricmp(owner, self_mod) == 0;
        sh_printf("  %-16s -> %s%s\n", name, owner[0] ? owner : "(unresolved module)",
                  is_backend ? " (this backend)" : "");
        if (is_backend) backend_owned++; else other_owned++;
    }
    char summary[220];
    _snprintf_s(summary, sizeof summary, _TRUNCATE,
        "snapstack_diag: %d/%d backend-owned, %d owned by another module, %d missing",
        backend_owned, SNAPSTACK_COMMAND_COUNT, other_owned, missing);
    sh_printf("%s\n", summary);
    backend_log(summary);
    ic_toast(iface, "SnapStack", summary);
}

void sh_register_snapstack_commands_backend(sh_iface *iface)
{
    if (!iface || !iface->vtbl || !iface->vtbl->register_cmd) return;
    for (int i = 0; i < SNAPSTACK_COMMAND_COUNT; i++)
        iface->vtbl->register_cmd(iface, SNAPSTACK_COMMANDS[i].name, SNAPSTACK_COMMANDS[i].handler, iface);
}

void sh_snapstack_push_ids_backend(int index, const int *ids, int count)
{
    if (count > 0) stack_push(index, ids, count);
}

int sh_snapstack_clear_stack_backend(int index)
{
    int had = stack_get(index)->count;
    stack_clear(index);
    return had;
}
