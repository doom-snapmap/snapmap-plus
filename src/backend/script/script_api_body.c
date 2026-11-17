/* script_api_body.c -- sync_partner, wounds_open and break_constraint
 * (script_api_body.h). The first half reads through the game's type
 * information and the engine table, so the pure test runs it over fake
 * types; the second half, without SH_SCRIPT_PURE, resolves the one engine
 * call.
 *
 * Build 13954591 facts (shared Ghidra, snaphak-re logs/shared-ghidra/gk2/):
 *   sync           idSyncEntity.activeParticipants, idList < idActiveSyncParticipant >: instance (an entity
 *                  reference), data (idSyncParticipant *, its type: INSTIGATOR 0, TARGET 1, HELPER 2). A
 *                  participant entity carries no link to its sync, so the map's sync entities are asked.
 *   wounds         goreComponent.activeWounds and .pendingWounds, idList < woundData_t >; woundNode is the
 *                  idDeclGoreNode whose name (+8, the name ae_goreEnableByName takes) the call answers.
 *   figure         an idAnimator_AF field (afProperties on idGoreEntity) .physicsObj (idPhysics_AF)
 *                  .constraints, idList < idAFConstraint * >, each with its idAtomicString name (+0x10, the
 *                  interned text pointer). DeleteConstraint VK 0x141643050 / GL 0x141643B60 (physics,
 *                  idAtomicString *) finds the constraint by that pointer, deletes it, closes the list and
 *                  sets changedAF (+0x2D0) so the figure rebuilds its trees; idAnimator_AF's own bind
 *                  constraint removal (VK 0x141659200) calls it the same way at run time. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "script_api_common.h"
#include "script_api_reflect.h"
#include "script_api_body.h"

#define BODY_LIST_CAP   256     /* participants, wounds or constraints read from one list */
#define BODY_NAME_MAX   256

static sh_body_delete_fn g_delete;

void sh_script_api_body_set_delete(sh_body_delete_fn fn) { g_delete = fn; }

/* ---- the walk ---- */

static int at(const sh_rf_host *h, const void *obj, const char *cls, const char *path, sh_rf_loc *out)
{
    char err[160];
    return obj && cls && sh_rf_resolve(h, obj, cls, path, out, err, sizeof err);
}

static const void *read_ptr(const sh_rf_host *h, const uint8_t *addr)
{
    const void *p = NULL;
    return h->read(h->ctx, addr, (void *)&p, sizeof p) ? p : NULL;
}

/* The entity behind a reference field, by its spawn id. */
static void *ref_entity(const sh_rf_host *h, const sh_rf_loc *loc)
{
    const sh_script_engine *e = sh_script_api_engine();
    uint32_t id = SH_SCRIPT_SPAWN_ID_NONE;
    if (loc->kind != SH_RF_REF || !e || !e->resolve) return NULL;
    if (!h->read(h->ctx, loc->addr + SH_RF_REF_SPAWN_ID, &id, sizeof id)) return NULL;
    return e->resolve(id);
}

static int class_has_field(const sh_rf_host *h, const char *cls, const char *name)
{
    sh_rf_field f;
    return cls && sh_rf_find_field(h, cls, name, &f);
}

int sh_script_body_has_gore(void *ent)
{
    const sh_rf_host *h = sh_script_reflect_host();
    const char *cls = h ? sh_script_reflect_class_of(ent) : NULL;
    return h && class_has_field(h, cls, "goreComponent");
}

/* ---- sync_partner ---- */

/* Classes met while scanning, by name: 1 a sync entity, 0 not. */
#define BODY_CLASS_CACHE 64
static struct { char *cls; int sync; } g_cls[BODY_CLASS_CACHE];
static int g_ncls;

static int is_sync_class(const char *cls)
{
    int i, v;
    for (i = 0; i < g_ncls; i++) if (!strcmp(g_cls[i].cls, cls)) return g_cls[i].sync;
    v = sh_script_reflect_is_a(cls, "idSyncEntity");
    if (g_ncls < BODY_CLASS_CACHE && (g_cls[g_ncls].cls = _strdup(cls)) != NULL) { g_cls[g_ncls].sync = v; g_ncls++; }
    return v;
}

typedef struct { void *self, *target, *instigator; int found; } sync_read;

/* Read one sync entity's active participants; 1 when `self` is one of them. */
static int read_sync(const sh_rf_host *h, void *sync, const char *cls, sync_read *r)
{
    sh_rf_loc list;
    long long n;
    uint32_t i;
    long long target_v = 1, instigator_v = 0;
    int mine = 0;
    void *target = NULL, *instigator = NULL;
    if (!at(h, sync, cls, "activeParticipants", &list)) return 0;
    n = list.kind == SH_RF_LIST ? sh_rf_count(h, &list) : -1;
    for (i = 0; n > 0 && i < (uint32_t)n && i < BODY_LIST_CAP; i++) {
        sh_rf_loc el, inst, data, type;
        char err[160];
        void *who;
        const void *pd;
        long long v = -1;
        int32_t raw = -1;
        if (!sh_rf_element(h, &list, i, &el, err, sizeof err)) break;
        if (!at(h, el.addr, el.type, "instance", &inst)) { sh_rf_loc_free(&el); continue; }
        who = ref_entity(h, &inst);
        sh_rf_loc_free(&inst);
        if (at(h, el.addr, el.type, "data", &data)) {
            pd = data.kind == SH_RF_POINTER ? read_ptr(h, data.addr) : NULL;
            if (pd && at(h, pd, data.type, "type", &type)) {
                if (type.kind == SH_RF_ENUM && h->read(h->ctx, type.addr, &raw, sizeof raw)) v = raw;
                if (type.type && h->enum_value) {
                    h->enum_value(h->ctx, type.type, "TARGET", &target_v);
                    h->enum_value(h->ctx, type.type, "INSTIGATOR", &instigator_v);
                }
                sh_rf_loc_free(&type);
            }
            sh_rf_loc_free(&data);
        }
        sh_rf_loc_free(&el);
        if (!who) continue;
        if (who == r->self) mine = 1;
        if (v == target_v && !target) target = who;
        else if (v == instigator_v && !instigator) instigator = who;
    }
    sh_rf_loc_free(&list);
    if (!mine) return 0;
    r->target = target;
    r->instigator = instigator;
    r->found = 1;
    return 1;
}

/* e:sync_partner() -> target, instigator, sync | nil, why */
static int l_sync_partner(lua_State *L)
{
    void *ent = sh_script_api_entity(L, 1, "sync_partner", NULL);
    const sh_rf_host *h = sh_script_reflect_host();
    const sh_script_engine *e = sh_script_api_engine();
    sync_read r;
    int i, n;
    if (!ent) { lua_pushnil(L); lua_pushstring(L, "the entity is gone"); return 2; }
    if (!h || !e || !e->slots || !e->slot_spawn_id || !e->resolve) {
        lua_pushnil(L);
        lua_pushstring(L, "the game's type information could not be read in this session");
        return 2;
    }
    memset(&r, 0, sizeof r);
    r.self = ent;
    n = e->slots();
    for (i = 0; i < n; i++) {
        uint32_t id = e->slot_spawn_id(i);
        void *s;
        const char *cls;
        if (id == SH_SCRIPT_SPAWN_ID_NONE || !(s = e->resolve(id))) continue;
        if (!(cls = sh_script_reflect_class_of(s)) || !is_sync_class(cls)) continue;
        if (read_sync(h, s, cls, &r)) {
            sh_script_api_push_engine_entity(L, r.target);
            sh_script_api_push_engine_entity(L, r.instigator);
            sh_script_api_push_engine_entity(L, s);
            return 3;
        }
    }
    lua_pushnil(L);
    lua_pushstring(L, "the entity is in no sync now");
    return 2;
}

/* ---- wounds_open ---- */

static int push_wounds(lua_State *L, const sh_rf_host *h, const sh_rf_loc *comp, const char *field, int k)
{
    sh_rf_loc list;
    long long n;
    uint32_t i;
    if (!at(h, comp->addr, comp->type, field, &list)) return k;
    n = list.kind == SH_RF_LIST ? sh_rf_count(h, &list) : -1;
    for (i = 0; n > 0 && i < (uint32_t)n && i < BODY_LIST_CAP; i++) {
        sh_rf_loc el, node;
        char err[160];
        const void *pn;
        const char *name;
        if (!sh_rf_element(h, &list, i, &el, err, sizeof err)) break;
        if (at(h, el.addr, el.type, "woundNode", &node)) {
            pn = node.kind == SH_RF_POINTER ? read_ptr(h, node.addr) : NULL;
            name = pn ? sh_script_reflect_name_of(pn) : NULL;
            if (name && name[0]) {
                int j, dup = 0;
                for (j = 1; j <= k && !dup; j++) {      /* one entry a node: a wound pending and open */
                    lua_rawgeti(L, -1, j);
                    dup = !strcmp(lua_tostring(L, -1), name);
                    lua_pop(L, 1);
                }
                if (!dup) { lua_pushstring(L, name); lua_rawseti(L, -2, ++k); }
            }
            sh_rf_loc_free(&node);
        }
        sh_rf_loc_free(&el);
    }
    sh_rf_loc_free(&list);
    return k;
}

/* e:wounds_open() -> { node, ... } (open first, then those about to show) | nil, why */
static int l_wounds_open(lua_State *L)
{
    void *ent = sh_script_api_entity(L, 1, "wounds_open", NULL);
    const sh_rf_host *h = sh_script_reflect_host();
    const char *cls;
    sh_rf_loc comp;
    int k = 0;
    if (!ent) { lua_pushnil(L); lua_pushstring(L, "the entity is gone"); return 2; }
    if (!h) { lua_pushnil(L); lua_pushstring(L, "the game's type information could not be read in this session"); return 2; }
    cls = sh_script_reflect_class_of(ent);
    if (!class_has_field(h, cls, "goreComponent") || !at(h, ent, cls, "goreComponent", &comp)) {
        lua_pushnil(L);
        lua_pushstring(L, "this entity has no gore component");
        return 2;
    }
    lua_newtable(L);
    k = push_wounds(L, h, &comp, "activeWounds", k);
    k = push_wounds(L, h, &comp, "pendingWounds", k);
    sh_rf_loc_free(&comp);
    (void)k;
    return 1;
}

/* ---- break_constraint ---- */

/* The entity's articulated figure: the first idAnimator_AF field of its class
 * chain (afProperties on idGoreEntity). 1 with its physics object. */
static int figure_physics(const sh_rf_host *h, void *ent, const char *cls, sh_rf_loc *physics)
{
    const char *c = cls;
    int guard = 0;
    if (class_has_field(h, cls, "afProperties") && at(h, ent, cls, "afProperties.physicsObj", physics)) return 1;
    while (c && c[0] && guard++ < 64) {
        sh_rf_field f;
        uint32_t i;
        const char *super;
        for (i = 0; h->class_field && h->class_field(h->ctx, c, i, &f); i++) {
            char path[BODY_NAME_MAX];
            if (!f.type || strcmp(f.type, "idAnimator_AF") || (f.ops && f.ops[0]) || !f.name) continue;
            snprintf(path, sizeof path, "%s.physicsObj", f.name);
            if (at(h, ent, cls, path, physics)) return 1;
        }
        super = h->class_super ? h->class_super(h->ctx, c) : NULL;
        if (!super || !strcmp(super, c)) break;
        c = super;
    }
    return 0;
}

/* e:break_constraint(name, [impulse]) -> true | false, why */
static int l_break_constraint(lua_State *L)
{
    const char *want = sh_script_api_check_text(L, 2, "break_constraint", "the constraint's name", BODY_NAME_MAX);
    float impulse[3];
    void *ent;
    const sh_rf_host *h;
    const char *cls;
    sh_rf_loc physics, list;
    long long n;
    uint32_t i;
    int found = 0, ran = 0;
    (void)sh_script_api_opt_vec(L, 3, "break_constraint", "the impulse", impulse);
    ent = sh_script_api_entity(L, 1, "break_constraint", NULL);
    h = sh_script_reflect_host();
    if (!ent) { lua_pushboolean(L, 0); lua_pushstring(L, "the entity is gone"); return 2; }
    if (!h) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "the game's type information could not be read in this session");
        return 2;
    }
    cls = sh_script_reflect_class_of(ent);
    if (!figure_physics(h, ent, cls, &physics)) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "this entity has no articulated figure");
        return 2;
    }
    if (!g_delete) {
        sh_rf_loc_free(&physics);
        lua_pushboolean(L, 0);
        lua_pushstring(L, "this Snapmap+ could not find the game's constraint removal in this executable");
        return 2;
    }
    if (!at(h, physics.addr, physics.type, "constraints", &list) || list.kind != SH_RF_LIST) {
        sh_rf_loc_free(&physics);
        lua_pushboolean(L, 0);
        lua_pushstring(L, "the articulated figure's constraints could not be read");
        return 2;
    }
    n = sh_rf_count(h, &list);
    for (i = 0; n > 0 && i < (uint32_t)n && i < BODY_LIST_CAP && !found; i++) {
        sh_rf_loc el, name;
        char err[160];
        const void *c;
        if (!sh_rf_element(h, &list, i, &el, err, sizeof err)) break;
        c = el.kind == SH_RF_POINTER ? read_ptr(h, el.addr) : NULL;
        if (c && at(h, c, el.type, "name", &name)) {
            const char *text = (const char *)read_ptr(h, name.addr);   /* idAtomicString: the interned text */
            char copy[BODY_NAME_MAX];
            if (text && h->read(h->ctx, text, copy, 1)) {
                size_t k;
                for (k = 0; k + 1 < sizeof copy && h->read(h->ctx, text + k, copy + k, 1) && copy[k]; k++) { }
                copy[k] = 0;
                if (!_stricmp(copy, want)) {
                    found = 1;
                    ran = g_delete((void *)physics.addr, (void *)name.addr);
                }
            }
            sh_rf_loc_free(&name);
        }
        sh_rf_loc_free(&el);
    }
    sh_rf_loc_free(&list);
    sh_rf_loc_free(&physics);
    if (found && ran) { lua_pushboolean(L, 1); return 1; }
    lua_pushboolean(L, 0);
    if (found) lua_pushstring(L, "the game's constraint removal could not run");
    else lua_pushfstring(L, "the articulated figure has no constraint named \"%s\" (it may be broken already)", want);
    return 2;
}

void sh_script_api_body_register(void)
{
    static int done;
    if (done) return;
    done = 1;
    sh_script_api_add(SH_SCRIPT_API_METHOD, "sync_partner", l_sync_partner);
    sh_script_api_add(SH_SCRIPT_API_METHOD, "wounds_open", l_wounds_open);
    sh_script_api_add(SH_SCRIPT_API_METHOD, "break_constraint", l_break_constraint);
}

#ifndef SH_SCRIPT_PURE
/* ============================================================ the engine */
#include <windows.h>
#include "../signatures.h"
#include "script_api_ai.h"
#include "../backend_log.h"

typedef void (*af_delete_fn)(void *physics, void *name);
static af_delete_fn g_af_delete;

static int e_delete(void *physics, void *name_field)
{
    if (!g_af_delete || !physics || !name_field) return 0;
    __try { g_af_delete(physics, name_field); }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return 0;
    }
    return 1;
}

int sh_script_api_body_install(const uint8_t *module_base)
{
    int i;
    sh_script_api_body_register();
    sh_script_ai_set_gore_owner_test(sh_script_body_has_gore);
    for (i = 0; module_base && SH_SCRIPT_API_SIGNATURES[i].name; i++) {
        sig_result r;
        if (strcmp(SH_SCRIPT_API_SIGNATURES[i].name, "ScriptAfDeleteConstraint")) continue;
        if (sig_resolve_one(module_base, &SH_SCRIPT_API_SIGNATURES[i], &r) == SIG_OK) g_af_delete = (af_delete_fn)r.addr;
        break;
    }
    if (g_af_delete) sh_script_api_body_set_delete(e_delete);
    else backend_log("Mod scripts: the game's articulated-figure constraint removal was not found in this executable, "
                     "so break_constraint answers false.");
    return g_af_delete != NULL;
}
#endif /* SH_SCRIPT_PURE */
