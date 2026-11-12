/* script_fsm_test.c -- script-backed classes for the game's AI state machines, against an
 * engine made here: four stock classes with records, objects and vtables shaped as the game's
 * (script_fsm.h), a lookup, and a script that answers what the test tells it to. */
#define SH_SCRIPT_PURE
#include "../src/backend/script/script_fsm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); g_fail++; } } while (0)

/* ---------------------------------------------------------------- the engine */

typedef struct obj { void **vt; int field; char body[0x200]; } obj;

static void *g_vt[SH_FSM_KINDS][SH_FSM_VTABLE_SLOTS];
static unsigned char g_rec[SH_FSM_KINDS][SH_FSM_RECORD_SIZE];
static int g_created, g_destroyed, g_cloned, g_stock_checks;
static int g_no_state_class;

static void *mk(int kind, int field) { obj *o = (obj *)calloc(1, sizeof *o); o->vt = g_vt[kind]; o->field = field; g_created++; return o; }
static void *create0(void) { return mk(0, 10); }
static void *create1(void) { return mk(1, 11); }
static void *create2(void) { return mk(2, 12); }
static void *create3(void) { return mk(3, 13); }
static void *clone_any(void *src)
{
    obj *s = (obj *)src, *o = (obj *)calloc(1, sizeof *o);
    int k;
    for (k = 0; k < SH_FSM_KINDS; k++) if (s->field == 10 + k) o->vt = g_vt[k];
    o->field = s->field;
    g_cloned++;
    return o;
}
static void *place_any(void *mem, int size) { obj *o = (obj *)mem; if (size < 16) return NULL; o->vt = g_vt[2]; o->field = 12; return o; }
static void destroy_any(void *o) { if (o) { g_destroyed++; free(o); } }

static void *stock_typeinfo(void *self) { (void)self; return NULL; }
static void *stock_check(void *self, void *out, void *ai, void *fsm, void *a5, void *a6, void *a7)
{
    void **o = (void **)out;
    (void)self; (void)ai; (void)fsm; (void)a5; (void)a6; (void)a7;
    g_stock_checks++;
    o[0] = (void *)0x7777;                  /* the answer's own vtable */
    *(int *)((unsigned char *)out + SH_FSM_CODE_OFFSET) = 0x1fa;
    o[2] = NULL;
    o[3] = NULL;
    return out;
}
static void stock_nothing(void) { }

static void engine_make(void)
{
    static void *(*const creates[SH_FSM_KINDS])(void) = { create0, create1, create2, create3 };
    int k, i;
    for (k = 0; k < SH_FSM_KINDS; k++) {
        void *p;
        for (i = 0; i < SH_FSM_VTABLE_SLOTS; i++) g_vt[k][i] = (void *)stock_nothing;
        g_vt[k][SH_FSM_SLOT_TYPEINFO] = (void *)stock_typeinfo;
        g_vt[2][SH_FSM_SLOT_CHECK] = (void *)stock_check;
        memset(g_rec[k], 0xA0 + k, SH_FSM_RECORD_SIZE);             /* the rest of a record is copied as it is */
        p = (void *)SH_FSM_SIBLINGS[k]; memcpy(g_rec[k] + SH_FSM_REC_NAME, &p, 8);
        p = (void *)creates[k];         memcpy(g_rec[k] + SH_FSM_REC_CREATE, &p, 8);
        p = (void *)clone_any;          memcpy(g_rec[k] + SH_FSM_REC_CLONE, &p, 8);
        p = (void *)place_any;          memcpy(g_rec[k] + SH_FSM_REC_PLACE, &p, 8);
        p = (void *)destroy_any;        memcpy(g_rec[k] + SH_FSM_REC_DESTROY, &p, 8);
    }
}

static void *f_lookup(const char *name)
{
    int k;
    for (k = 0; k < SH_FSM_KINDS; k++)
        if (!strcmp(name, SH_FSM_SIBLINGS[k])) return (k == SH_FSM_STATE && g_no_state_class) ? NULL : g_rec[k];
    if (!strcmp(name, "idAI2")) return (void *)0x1234;
    return NULL;
}

/* ---------------------------------------------------------------- the script */

static int g_on_thread = 1, g_calls, g_logs;
static sh_script_fsm_request g_seen;
static char g_seen_salt[32], g_seen_name[128], g_seen_hook[16], g_log[512];
static int g_answer_found = 1, g_answer_truth, g_answer_has_code, g_answer_code;

static int f_on_thread(void) { return g_on_thread; }
static uint64_t f_handle(void *ai) { return ai ? (uint64_t)(uintptr_t)ai + 1000 : 0; }
static int f_call(const char *salt, sh_script_fsm_request *rq)
{
    g_calls++;
    g_seen = *rq;
    strcpy_s(g_seen_salt, sizeof g_seen_salt, salt);
    strcpy_s(g_seen_name, sizeof g_seen_name, rq->name);
    strcpy_s(g_seen_hook, sizeof g_seen_hook, rq->hook ? rq->hook : "");
    rq->found = g_answer_found;
    rq->truth = g_answer_truth;
    rq->has_code = g_answer_has_code;
    rq->code = g_answer_code;
    return g_answer_found;
}
static void f_log(const char *line) { g_logs++; strcpy_s(g_log, sizeof g_log, line); }

static const sh_fsm_ops k_ops = { f_lookup, f_on_thread, f_handle, f_call, f_log };

/* ---------------------------------------------------------------- helpers */

typedef void *(*create_fn)(void);
typedef void *(*clone_fn)(void *);
typedef void *(*place_fn)(void *, int);
static void *rec_fn(const void *rec, int off) { void *p; memcpy(&p, (const unsigned char *)rec + off, 8); return p; }
static void *slot(void *o, int n) { return (*(void ***)o)[n]; }

/* ---------------------------------------------------------------- tests */

static void test_names(void)
{
    char salt[SH_FSM_SALT_CAP], cls[SH_FSM_CLASS_CAP];
    int kind = -1, idx = -1;
    CHECK(sh_fsm_parse("smp.condition.wuahwfly.ATRON_UseGrenades.3", &kind, salt, sizeof salt, cls, sizeof cls, &idx));
    CHECK(kind == SH_FSM_CONDITION && !strcmp(salt, "wuahwfly") && !strcmp(cls, "ATRON_UseGrenades") && idx == 3);
    CHECK(sh_fsm_parse("smp.state.ab12cd34.idArachnotronCombat_Hangout", &kind, salt, sizeof salt, cls, sizeof cls, &idx));
    CHECK(kind == SH_FSM_STATE && idx == 0);
    CHECK(sh_fsm_parse("smp.transition.ab12cd34.X.12", &kind, salt, sizeof salt, cls, sizeof cls, &idx) && idx == 12);
    CHECK(sh_fsm_parse("smp.snippet.ab12cd34.Y.0", &kind, salt, sizeof salt, cls, sizeof cls, &idx) && kind == SH_FSM_SNIPPET);
    /* not a script class name */
    CHECK(!sh_fsm_parse("idAICondition_Shared_HasTempMemoryKey", &kind, salt, sizeof salt, cls, sizeof cls, &idx));
    CHECK(!sh_fsm_parse("smp.", &kind, salt, sizeof salt, cls, sizeof cls, &idx));
    CHECK(!sh_fsm_parse("smp.action.ab12cd34.X.1", &kind, salt, sizeof salt, cls, sizeof cls, &idx));        /* no such kind */
    CHECK(!sh_fsm_parse("smp.condition.AB12CD34.X.1", &kind, salt, sizeof salt, cls, sizeof cls, &idx));     /* a salt is lower case */
    CHECK(!sh_fsm_parse("smp.condition.ab12cd34..1", &kind, salt, sizeof salt, cls, sizeof cls, &idx));
    CHECK(!sh_fsm_parse("smp.condition.ab12cd34.X.", &kind, salt, sizeof salt, cls, sizeof cls, &idx));
    CHECK(!sh_fsm_parse("smp.condition.ab12cd34.X.1a", &kind, salt, sizeof salt, cls, sizeof cls, &idx));
    CHECK(!sh_fsm_parse("smp.condition.ab12cd34.a-b.1", &kind, salt, sizeof salt, cls, sizeof cls, &idx));
    CHECK(!sh_fsm_parse(NULL, &kind, salt, sizeof salt, cls, sizeof cls, &idx));
}

static void test_lookup(void)
{
    void *a, *b;
    int before = sh_fsm_class_count();
    CHECK(sh_fsm_lookup("idAI2") == (void *)0x1234);          /* every other name is the engine's own answer */
    CHECK(sh_fsm_lookup("no_such_class") == NULL);
    CHECK(sh_fsm_lookup("smp.widget.ab12cd34.X.1") == NULL);  /* a bad script name is no class */
    a = sh_fsm_lookup("smp.condition.ab12cd34.Sees.1");
    CHECK(a != NULL && a != g_rec[SH_FSM_CONDITION]);
    CHECK(!strcmp((const char *)rec_fn(a, SH_FSM_REC_NAME), "smp.condition.ab12cd34.Sees.1"));
    /* the rest of the record is the stock class's: the engine's hierarchy tests read it */
    CHECK(((const unsigned char *)a)[0x68] == 0xA0 + SH_FSM_CONDITION && ((const unsigned char *)a)[0x50] == 0xA0);
    CHECK(rec_fn(a, SH_FSM_REC_CREATE) != rec_fn(g_rec[SH_FSM_CONDITION], SH_FSM_REC_CREATE));
    CHECK(rec_fn(a, SH_FSM_REC_DESTROY) == (void *)destroy_any);
    b = sh_fsm_lookup("smp.condition.ab12cd34.Sees.1");
    CHECK(a == b);                                            /* one record per name */
    CHECK(sh_fsm_lookup("smp.condition.ab12cd34.Sees.2") != a);
    CHECK(sh_fsm_class_count() == before + 2);
    g_no_state_class = 1;                                     /* a game without the stock class to copy */
    CHECK(sh_fsm_lookup("smp.state.ab12cd34.Hang.1") == NULL);
    g_no_state_class = 0;
    CHECK(sh_fsm_lookup("smp.state.ab12cd34.Hang.1") != NULL);
}

static void test_condition_and_snippet(void)
{
    void *rec = sh_fsm_lookup("smp.condition.wuahwfly.UseGrenades.4"), *o, *c;
    int destroyed = g_destroyed;
    o = ((create_fn)rec_fn(rec, SH_FSM_REC_CREATE))();
    CHECK(o && ((obj *)o)->field == 10);                       /* the stock object, as the engine allocates it */
    CHECK(((void *(*)(void *))slot(o, SH_FSM_SLOT_TYPEINFO))(o) == rec);
    g_calls = 0; g_answer_found = 1; g_answer_truth = 1; g_answer_has_code = 0;
    CHECK(((unsigned char (*)(void *, void *, void *, void *))slot(o, SH_FSM_SLOT_CALL))(o, (void *)50, NULL, NULL) == 1);
    CHECK(g_calls == 1 && g_seen.kind == SH_FSM_CONDITION && g_seen.index == 4 && g_seen.self == 1050);
    CHECK(!strcmp(g_seen_salt, "wuahwfly") && !strcmp(g_seen_name, "UseGrenades") && !g_seen_hook[0]);
    g_answer_truth = 0;
    CHECK(((unsigned char (*)(void *, void *, void *, void *))slot(o, SH_FSM_SLOT_CALL))(o, (void *)50, NULL, NULL) == 0);
    g_answer_found = 0; g_answer_truth = 1;                    /* the mod has no such class: no */
    CHECK(((unsigned char (*)(void *, void *, void *, void *))slot(o, SH_FSM_SLOT_CALL))(o, (void *)50, NULL, NULL) == 0);
    /* another thread: nothing is asked, the answer is no, one line says so */
    g_answer_found = 1; g_on_thread = 0; g_calls = 0; g_logs = 0;
    CHECK(((unsigned char (*)(void *, void *, void *, void *))slot(o, SH_FSM_SLOT_CALL))(o, (void *)50, NULL, NULL) == 0);
    CHECK(((unsigned char (*)(void *, void *, void *, void *))slot(o, SH_FSM_SLOT_CALL))(o, (void *)50, NULL, NULL) == 0);
    CHECK(g_calls == 0 && g_logs == 1 && strstr(g_log, "a thread other than the game's") != NULL);
    g_on_thread = 1;
    /* a clone is the same class */
    c = ((clone_fn)rec_fn(rec, SH_FSM_REC_CLONE))(o);
    CHECK(c && c != o && *(void ***)c == *(void ***)o && ((obj *)c)->field == 10);
    CHECK(((void *(*)(void *))slot(c, SH_FSM_SLOT_TYPEINFO))(c) == rec);
    ((void (*)(void *))rec_fn(rec, SH_FSM_REC_DESTROY))(c);
    ((void (*)(void *))rec_fn(rec, SH_FSM_REC_DESTROY))(o);
    CHECK(g_destroyed == destroyed + 2);

    rec = sh_fsm_lookup("smp.snippet.wuahwfly.SelectTurn.1");
    o = ((create_fn)rec_fn(rec, SH_FSM_REC_CREATE))();
    g_calls = 0;
    ((void (*)(void *, void *, void *, void *))slot(o, SH_FSM_SLOT_CALL))(o, NULL, NULL, NULL);
    CHECK(g_calls == 1 && g_seen.kind == SH_FSM_SNIPPET && g_seen.self == 0 && !strcmp(g_seen_name, "SelectTurn"));
    free(o);
}

static void test_transition(void)
{
    void *rec = sh_fsm_lookup("smp.transition.wuahwfly.ShouldTurn.2"), *o;
    void *out[4];
    char mem[64];
    o = ((create_fn)rec_fn(rec, SH_FSM_REC_CREATE))();
    g_stock_checks = 0; g_answer_found = 1; g_answer_truth = 1; g_answer_has_code = 1; g_answer_code = 0x2c;
    memset(out, 0xEE, sizeof out);
    CHECK(((void *(*)(void *, void *, void *, void *, void *, void *, void *))slot(o, SH_FSM_SLOT_CHECK))(o, out, (void *)7, NULL, NULL, NULL, NULL) == out);
    CHECK(g_stock_checks == 1 && out[0] == (void *)0x7777);    /* the answer is the engine's own object */
    CHECK(*(int *)((unsigned char *)out + SH_FSM_CODE_OFFSET) == 0x2c);
    CHECK(!strcmp((const char *)out[3], "ShouldTurn"));        /* the reason the engine logs */
    CHECK(g_seen.kind == SH_FSM_TRANSITION && g_seen.index == 2 && g_seen.self == 1007);
    g_answer_has_code = 0; g_answer_truth = 0;                 /* false: no code, the link does not fire */
    (void)((void *(*)(void *, void *, void *, void *, void *, void *, void *))slot(o, SH_FSM_SLOT_CHECK))(o, out, (void *)7, NULL, NULL, NULL, NULL);
    CHECK(*(int *)((unsigned char *)out + SH_FSM_CODE_OFFSET) == 0);
    g_answer_truth = 1; g_logs = 0;                            /* true is not a code: nothing fires, one line */
    (void)((void *(*)(void *, void *, void *, void *, void *, void *, void *))slot(o, SH_FSM_SLOT_CHECK))(o, out, (void *)7, NULL, NULL, NULL, NULL);
    (void)((void *(*)(void *, void *, void *, void *, void *, void *, void *))slot(o, SH_FSM_SLOT_CHECK))(o, out, (void *)7, NULL, NULL, NULL, NULL);
    CHECK(*(int *)((unsigned char *)out + SH_FSM_CODE_OFFSET) == 0 && g_logs == 1 && strstr(g_log, "answered true") != NULL);
    g_answer_found = 0;                                        /* no such class in the mod: code 0, not the stock code */
    (void)((void *(*)(void *, void *, void *, void *, void *, void *, void *))slot(o, SH_FSM_SLOT_CHECK))(o, out, (void *)7, NULL, NULL, NULL, NULL);
    CHECK(*(int *)((unsigned char *)out + SH_FSM_CODE_OFFSET) == 0);
    free(o);
    /* built in place */
    o = ((place_fn)rec_fn(rec, SH_FSM_REC_PLACE))(mem, (int)sizeof mem);
    CHECK(o == (void *)mem && ((void *(*)(void *))slot(o, SH_FSM_SLOT_TYPEINFO))(o) == rec);
    CHECK(((place_fn)rec_fn(rec, SH_FSM_REC_PLACE))(mem, 4) == NULL);
}

static void test_state(void)
{
    void *rec = sh_fsm_lookup("smp.state.wuahwfly.Hangout.1"), *o;
    unsigned char *machine = (unsigned char *)calloc(1, SH_FSM_WORK_CODE + 16);
    int *work = (int *)(machine + SH_FSM_WORK_CODE);
    o = ((create_fn)rec_fn(rec, SH_FSM_REC_CREATE))();
    g_answer_found = 1; g_answer_truth = 0; g_answer_has_code = 0; g_calls = 0;
    ((void (*)(void *, void *, void *, void *, void *))slot(o, SH_FSM_SLOT_ENTER))(o, (void *)9, machine, NULL, NULL);
    CHECK(g_calls == 1 && !strcmp(g_seen_hook, "enter") && g_seen.kind == SH_FSM_STATE && *work == 0);
    g_answer_has_code = 1; g_answer_code = 0xf;                /* update hands its machine a code */
    ((void (*)(void *, void *, void *, void *))slot(o, SH_FSM_SLOT_UPDATE))(o, (void *)9, machine, NULL);
    CHECK(!strcmp(g_seen_hook, "update") && *work == 0xf);
    *work = 0; g_answer_code = 0x10;                           /* exit hands none */
    ((void (*)(void *, void *, void *, void *))slot(o, SH_FSM_SLOT_EXIT))(o, (void *)9, machine, NULL);
    CHECK(!strcmp(g_seen_hook, "exit") && *work == 0);
    g_answer_code = 0;                                         /* 0 is no code: the machine's is left alone */
    *work = 5;
    ((void (*)(void *, void *, void *, void *))slot(o, SH_FSM_SLOT_UPDATE))(o, (void *)9, machine, NULL);
    CHECK(*work == 5);
    /* every other virtual is the stock class's own */
    CHECK(slot(o, 3) == (void *)stock_nothing && slot(o, 1) == (void *)stock_nothing && slot(o, 27) == (void *)stock_nothing);
    free(o);
    free(machine);
}

int main(void)
{
    engine_make();
    sh_fsm_set_ops(&k_ops);
    test_names();
    test_lookup();
    test_condition_and_snippet();
    test_transition();
    test_state();
    printf("script_fsm_test: %s\n", g_fail ? "FAILED" : "ok");
    return g_fail ? 1 : 0;
}
