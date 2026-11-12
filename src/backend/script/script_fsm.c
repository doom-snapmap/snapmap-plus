/* script_fsm.c -- script-backed classes for the game's AI state machines (script_fsm.h). */
#include "script_fsm.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *const SH_FSM_KIND_NAMES[SH_FSM_KINDS] = { "condition", "snippet", "transition", "state" };
const char *const SH_FSM_SIBLINGS[SH_FSM_KINDS] = {
    "idAICondition_Shared_HasTempMemoryKey",   /* a condition with one field; Evaluate reads the AI only */
    "idAISnippet_Shared_AbortWaveblast",       /* a snippet with no fields */
    "Shared_DefaultUse",                       /* Check fills the answer and reads nothing (VK 0x1405ED170) */
    "idShared_Passthrough_Immediate"           /* a state whose three hooks are the engine's empty function */
};

typedef void *(*create_fn)(void);
typedef void *(*clone_fn)(void *src);
typedef void *(*place_fn)(void *mem, int size);
typedef void *(*check_fn)(void *self, void *out, void *ai, void *fsm, void *a5, void *a6, void *a7);

/* One script class: the record the engine holds, then what the bridge knows about it. The
 * record comes first, so the engine's pointer to the record is the class. */
typedef struct fsm_class {
    unsigned char record[SH_FSM_RECORD_SIZE];
    int        kind, index;
    char       salt[SH_FSM_SALT_CAP];
    char       cls[SH_FSM_CLASS_CAP];
    char      *name;                 /* the whole name; the record's +0x00 points here */
    create_fn  sib_create;
    clone_fn   sib_clone;
    place_fn   sib_place;
    void     **sib_vtable;           /* the stock class's own table */
    void      *table[1 + SH_FSM_VTABLE_SLOTS];   /* [0] this class, then the copy the objects carry */
    struct fsm_class *next;
} fsm_class;

static const sh_fsm_ops *g_ops;
static SRWLOCK    g_lock = SRWLOCK_INIT;
static fsm_class *g_classes;
static int        g_nclasses;
static LONG       g_said_thread, g_said_code;

void sh_fsm_set_ops(const sh_fsm_ops *ops) { g_ops = ops; }

static void say(const char *line) { if (g_ops && g_ops->log) g_ops->log(line); }

static int name_char(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

int sh_fsm_parse(const char *name, int *kind, char *salt, size_t salt_cap, char *cls, size_t cls_cap, int *index)
{
    const char *p, *dot;
    size_t n;
    int k, idx = 0;
    if (!name || strncmp(name, SH_FSM_PREFIX, sizeof SH_FSM_PREFIX - 1)) return 0;
    p = name + sizeof SH_FSM_PREFIX - 1;
    dot = strchr(p, '.');
    if (!dot) return 0;
    for (k = 0; k < SH_FSM_KINDS; k++)
        if (strlen(SH_FSM_KIND_NAMES[k]) == (size_t)(dot - p) && !strncmp(p, SH_FSM_KIND_NAMES[k], (size_t)(dot - p))) break;
    if (k == SH_FSM_KINDS) return 0;
    p = dot + 1;
    dot = strchr(p, '.');
    n = dot ? (size_t)(dot - p) : 0;
    if (!n || n >= salt_cap) return 0;
    for (size_t i = 0; i < n; i++) if (!((p[i] >= 'a' && p[i] <= 'z') || (p[i] >= '0' && p[i] <= '9'))) return 0;
    memcpy(salt, p, n); salt[n] = 0;
    p = dot + 1;
    dot = strchr(p, '.');
    n = dot ? (size_t)(dot - p) : strlen(p);
    if (!n || n >= cls_cap) return 0;
    for (size_t i = 0; i < n; i++) if (!name_char((unsigned char)p[i])) return 0;
    memcpy(cls, p, n); cls[n] = 0;
    if (dot) {
        p = dot + 1;
        if (!*p) return 0;
        for (; *p; p++) {
            if (*p < '0' || *p > '9' || idx > 100000) return 0;
            idx = idx * 10 + (*p - '0');
        }
    }
    if (kind) *kind = k;
    if (index) *index = idx;
    return 1;
}

/* ---------------------------------------------------------------- the virtuals */

static fsm_class *class_of(void *self)
{
    void **vt = self ? *(void ***)self : NULL;
    return vt ? (fsm_class *)vt[-1] : NULL;
}

/* Ask the mod's script; 0 when it cannot be asked (another thread, no runtime, no class). */
static int ask(fsm_class *c, void *ai, const char *hook, sh_script_fsm_request *rq)
{
    memset(rq, 0, sizeof *rq);
    if (!c || !g_ops || !g_ops->call) return 0;
    if (g_ops->on_game_thread && !g_ops->on_game_thread()) {
        if (!InterlockedExchange(&g_said_thread, 1))
            say("SCRIPTS: a state-machine class of a mod was asked from a thread other than the game's; it answered "
                "nothing there (the map's AI state machines run on the game thread while such classes are in use)");
        return 0;
    }
    rq->kind = c->kind;
    rq->name = c->cls;
    rq->hook = hook;
    rq->index = c->index;
    rq->self = g_ops->handle_of ? g_ops->handle_of(ai) : 0;
    return g_ops->call(c->salt, rq) == 1 && rq->found;
}

static void *v_typeinfo(void *self)
{
    return class_of(self);   /* the record is the class's first member */
}

static unsigned char v_condition(void *self, void *ai, void *fsm, void *extra)
{
    sh_script_fsm_request rq;
    (void)fsm; (void)extra;
    return (unsigned char)(ask(class_of(self), ai, NULL, &rq) && rq.truth);
}

static void v_snippet(void *self, void *ai, void *fsm, void *extra)
{
    sh_script_fsm_request rq;
    (void)fsm; (void)extra;
    (void)ask(class_of(self), ai, NULL, &rq);
}

/* The stock class fills the answer (its vtable, a code, no reason); the script's code replaces
 * the code. No answer is code 0: the link does not fire. */
static void *v_check(void *self, void *out, void *ai, void *fsm, void *a5, void *a6, void *a7)
{
    fsm_class *c = class_of(self);
    sh_script_fsm_request rq;
    int code = 0;
    if (c && c->sib_vtable) (void)((check_fn)c->sib_vtable[SH_FSM_SLOT_CHECK])(self, out, ai, fsm, a5, a6, a7);
    if (ask(c, ai, NULL, &rq)) {
        if (rq.has_code) code = rq.code;
        else if (rq.truth && !InterlockedExchange(&g_said_code, 1))
            say("SCRIPTS: a transition class of a mod answered true; a transition answers the code of the link it "
                "fires (a number) or false, so that answer fired nothing");
    }
    if (out) {
        *(int *)((unsigned char *)out + SH_FSM_CODE_OFFSET) = code;
        *(const char **)((unsigned char *)out + SH_FSM_REASON_OFFSET) = c ? c->cls : NULL;
    }
    return out;
}

static void state_hook(void *self, void *ai, void *fsm, const char *hook)
{
    sh_script_fsm_request rq;
    if (ask(class_of(self), ai, hook, &rq) && rq.has_code && rq.code && fsm)
        *(int *)((unsigned char *)fsm + SH_FSM_WORK_CODE) = rq.code;
}

static void v_enter(void *self, void *ai, void *fsm, void *time, void *x)
{
    (void)time; (void)x;
    state_hook(self, ai, fsm, "enter");
}

static void v_exit(void *self, void *ai, void *fsm, void *time)
{
    sh_script_fsm_request rq;
    (void)time; (void)fsm;
    (void)ask(class_of(self), ai, "exit", &rq);
}

static void v_update(void *self, void *ai, void *fsm, void *time)
{
    (void)time;
    state_hook(self, ai, fsm, "update");
}

/* ---------------------------------------------------------------- objects */

static void *dress(fsm_class *c, void *obj)
{
    if (obj) *(void ***)obj = &c->table[1];
    return obj;
}

static void *class_create(fsm_class *c) { return dress(c, c->sib_create ? c->sib_create() : NULL); }
static void *class_clone(void *src, fsm_class *c) { return dress(c, c->sib_clone ? c->sib_clone(src) : NULL); }
static void *class_place(void *mem, int size, fsm_class *c) { return dress(c, c->sib_place ? c->sib_place(mem, size) : NULL); }

/* A record's create, clone and placement constructor take no word of their own, so each class
 * gets three stubs that load it into the next free argument register and jump to the common
 * function. One page per class, written, then made execute-read. */
static unsigned char *emit(unsigned char *p, int reg, void *cls, void *fn)
{
    static const unsigned char mov[3][2] = { { 0x48, 0xB9 }, { 0x48, 0xBA }, { 0x49, 0xB8 } };   /* rcx, rdx, r8 */
    memcpy(p, mov[reg], 2); memcpy(p + 2, &cls, 8);
    p[10] = 0x48; p[11] = 0xB8; memcpy(p + 12, &fn, 8);      /* mov rax, fn */
    p[20] = 0xFF; p[21] = 0xE0;                              /* jmp rax */
    return p + 24;
}

static int make_stubs(fsm_class *c)
{
    unsigned char *page = (unsigned char *)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE), *p;
    void *create, *clone, *place;
    DWORD old;
    if (!page) return 0;
    create = page;
    p = emit(page, 0, c, (void *)class_create);
    clone = p;
    p = emit(p, 1, c, (void *)class_clone);
    place = p;
    (void)emit(p, 2, c, (void *)class_place);
    if (!VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &old)) { VirtualFree(page, 0, MEM_RELEASE); return 0; }
    FlushInstructionCache(GetCurrentProcess(), page, 4096);
    memcpy(c->record + SH_FSM_REC_CREATE, &create, 8);
    memcpy(c->record + SH_FSM_REC_CLONE, &clone, 8);
    memcpy(c->record + SH_FSM_REC_PLACE, &place, 8);
    return 1;
}

/* The stock class's table, read from one object of it. */
static void **sibling_vtable(const unsigned char *sib)
{
    create_fn create;
    void (*destroy)(void *);
    void *obj, **vt = NULL;
    memcpy(&create, sib + SH_FSM_REC_CREATE, 8);
    memcpy(&destroy, sib + SH_FSM_REC_DESTROY, 8);
    if (!create) return NULL;
    __try {
        obj = create();
        if (obj) {
            vt = *(void ***)obj;
            if (destroy) destroy(obj);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { vt = NULL; }
    return vt;
}

static fsm_class *build(const char *name, int kind, const char *salt, const char *cls, int index)
{
    const unsigned char *sib = g_ops && g_ops->lookup ? (const unsigned char *)g_ops->lookup(SH_FSM_SIBLINGS[kind]) : NULL;
    fsm_class *c;
    void **vt;
    int ok = 0;
    if (!sib) return NULL;
    vt = sibling_vtable(sib);
    if (!vt) return NULL;
    c = (fsm_class *)calloc(1, sizeof *c);
    if (!c) return NULL;
    c->name = _strdup(name);
    if (!c->name) { free(c); return NULL; }
    __try {
        memcpy(c->record, sib, SH_FSM_RECORD_SIZE);
        memcpy(&c->table[1], vt, SH_FSM_VTABLE_SLOTS * sizeof(void *));
        ok = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    if (!ok) { free(c->name); free(c); return NULL; }
    c->kind = kind;
    c->index = index;
    strcpy_s(c->salt, sizeof c->salt, salt);
    strcpy_s(c->cls, sizeof c->cls, cls);
    memcpy(&c->sib_create, sib + SH_FSM_REC_CREATE, 8);
    memcpy(&c->sib_clone, sib + SH_FSM_REC_CLONE, 8);
    memcpy(&c->sib_place, sib + SH_FSM_REC_PLACE, 8);
    c->sib_vtable = vt;
    memcpy(c->record + SH_FSM_REC_NAME, &c->name, 8);
    c->table[0] = c;
    c->table[1 + SH_FSM_SLOT_TYPEINFO] = (void *)v_typeinfo;
    switch (kind) {
    case SH_FSM_CONDITION:  c->table[1 + SH_FSM_SLOT_CALL] = (void *)v_condition; break;
    case SH_FSM_SNIPPET:    c->table[1 + SH_FSM_SLOT_CALL] = (void *)v_snippet; break;
    case SH_FSM_TRANSITION: c->table[1 + SH_FSM_SLOT_CHECK] = (void *)v_check; break;
    default:
        c->table[1 + SH_FSM_SLOT_ENTER] = (void *)v_enter;
        c->table[1 + SH_FSM_SLOT_EXIT] = (void *)v_exit;
        c->table[1 + SH_FSM_SLOT_UPDATE] = (void *)v_update;
        break;
    }
    if (!make_stubs(c)) { free(c->name); free(c); return NULL; }
    return c;
}

void *sh_fsm_class(const char *name)
{
    char salt[SH_FSM_SALT_CAP], cls[SH_FSM_CLASS_CAP];
    int kind, index;
    fsm_class *c;
    if (!sh_fsm_parse(name, &kind, salt, sizeof salt, cls, sizeof cls, &index)) return NULL;
    AcquireSRWLockShared(&g_lock);
    for (c = g_classes; c; c = c->next) if (!strcmp(c->name, name)) break;
    ReleaseSRWLockShared(&g_lock);
    if (c) return c;
    AcquireSRWLockExclusive(&g_lock);
    for (c = g_classes; c; c = c->next) if (!strcmp(c->name, name)) break;
    if (!c) {
        /* never freed: a decl the engine parsed holds the record for as long as the game runs */
        c = build(name, kind, salt, cls, index);
        if (c) { c->next = g_classes; g_classes = c; g_nclasses++; }
    }
    ReleaseSRWLockExclusive(&g_lock);
    return c;
}

void *sh_fsm_lookup(const char *name)
{
    if (name && !strncmp(name, SH_FSM_PREFIX, sizeof SH_FSM_PREFIX - 1)) return sh_fsm_class(name);
    return g_ops && g_ops->lookup ? g_ops->lookup(name) : NULL;
}

int sh_fsm_class_count(void)
{
    int n;
    AcquireSRWLockShared(&g_lock);
    n = g_nclasses;
    ReleaseSRWLockShared(&g_lock);
    return n;
}

#ifndef SH_SCRIPT_PURE
/* ================================================== engine side (the DLL) */
#include "../backend_log.h"
#include "../hook.h"
#include "../signatures.h"
#include "script_hooks.h"

extern const sig_entry SH_SCRIPT_FSM_SIGNATURE;   /* script_fsm_sigs.c */

typedef void *(*lookup_fn)(const char *name);
static void *g_tramp;

static void *engine_lookup(const char *name) { return g_tramp ? ((lookup_fn)g_tramp)(name) : NULL; }
static void *lookup_detour(const char *name) { return sh_fsm_lookup(name); }
static int engine_on_game_thread(void) { return sh_script_hooks_on_game_thread(); }
static uint64_t engine_handle_of(void *ai) { int mod = -1; return ai ? sh_script_hooks_handle_of(ai, &mod) : 0; }
static void engine_log(const char *line) { backend_log(line); }

static const sh_fsm_ops k_engine_ops = { engine_lookup, engine_on_game_thread, engine_handle_of, sh_script_fsm_call,
                                         engine_log };

int sh_script_fsm_install(const uint8_t *module_base)
{
    sig_result r;
    void *tramp;
    if (g_tramp) return 1;
    if (!module_base || sig_resolve_one(module_base, &SH_SCRIPT_FSM_SIGNATURE, &r) != SIG_OK) {
        backend_log("SCRIPTS: state-machine classes from mods are unavailable in this session: the game function they "
                    "need (TypeInfoByName) was not found in this executable.");
        return 0;
    }
    tramp = hook_prepare((void *)r.addr, (void *)lookup_detour, SH_FSM_LOOKUP_STOLEN);
    if (!tramp) {
        backend_log("SCRIPTS: state-machine classes from mods are unavailable in this session: the game's type lookup "
                    "could not be prepared for its hook.");
        return 0;
    }
    g_tramp = tramp;                        /* published before the commit: the detour calls it */
    sh_fsm_set_ops(&k_engine_ops);
    if (hook_commit(tramp) != B2_PATCH_OK) {
        (void)hook_unpatch(tramp);
        g_tramp = NULL;
        backend_log("SCRIPTS: state-machine classes from mods are unavailable in this session: the game's type lookup "
                    "could not be hooked (another tool may have changed it).");
        return 0;
    }
    backend_log("SCRIPTS: state-machine classes from mods resolve through the game's type lookup");
    return 1;
}
#endif /* SH_SCRIPT_PURE */
