/* script_fsm.h -- classes a mod's script supplies to the game's own AI state machines.
 *
 * DOOM 2016 runs each demon on state machines read from aifsmmanager decls. Every state,
 * transition, condition and snippet of a graph names a class, and the engine finds the class
 * by name in its type registry (the lookup, VK 0x141583B20 / GL 0x141584BB0). A demon ported
 * from another game uses the engine's classes wherever the engine has them; the ones it lacks
 * are written in Lua and named in the decl as
 *
 *     smp.<kind>.<salt>.<class>.<n>      kind: condition, snippet, transition, state
 *                                        salt: the mod; class: the name the script registers;
 *                                        n: which parameter set of the class this node uses
 *
 * The lookup is detoured: such a name answers a record copied from a stock class of the same
 * kind (so the engine's own hierarchy tests hold), whose create and clone build the stock
 * object and give it a copy of the stock vtable in which the one virtual the kind implements
 * calls the mod's script. The engine skips the `object = { ... }` block of a class it has no
 * reflection record for, so a node's parameters are not read from the decl: they are the
 * script's own data, chosen by <n>.
 *
 * Engine facts (build 13954591, R-M6-engine-fsm): a type record is 0x90 bytes with its name at
 * +0x00, create at +0x28, clone at +0x30 and the placement constructor at +0x38; a condition
 * and a snippet implement vtable slot 2 (this, ai, fsm, extra), a transition slot 12 (this,
 * out, ai, fsm, ...) filling a transition code at out+8, a state slots 25 OnEnter, 26 OnExit
 * and 28 Update (this, ai, fsm, time); a state hands its machine a transition code by writing
 * it at fsm+0x6BD8 (idShared_FaceEnemy's Update, VK 0x1405E8A60). The decl's objects are
 * templates the engine clones per demon, so nothing about one demon is kept on an object.
 *
 * Scripts run on the game's thread only. A class asked from another thread answers "no" (a
 * condition), no code (a transition) or does nothing, and one line says so.
 */
#ifndef SH_SCRIPT_FSM_H
#define SH_SCRIPT_FSM_H

#include <stddef.h>
#include <stdint.h>

enum { SH_FSM_CONDITION, SH_FSM_SNIPPET, SH_FSM_TRANSITION, SH_FSM_STATE, SH_FSM_KINDS };

#define SH_FSM_PREFIX        "smp."
#define SH_FSM_RECORD_SIZE   0x90
#define SH_FSM_REC_NAME      0x00
#define SH_FSM_REC_CREATE    0x28
#define SH_FSM_REC_CLONE     0x30
#define SH_FSM_REC_PLACE     0x38
#define SH_FSM_REC_DESTROY   0x40
#define SH_FSM_SLOT_TYPEINFO 0
#define SH_FSM_SLOT_CALL     2      /* a condition's Evaluate, a snippet's Execute */
#define SH_FSM_SLOT_CHECK    12     /* a transition's Check */
#define SH_FSM_SLOT_ENTER    25
#define SH_FSM_SLOT_EXIT     26
#define SH_FSM_SLOT_UPDATE   28
#define SH_FSM_VTABLE_SLOTS  64     /* copied per class; a state's table is the longest (40 odd) */
#define SH_FSM_CODE_OFFSET   0x08   /* int: the code in a transition's answer */
#define SH_FSM_REASON_OFFSET 0x18   /* const char *: why, for the engine's log */
#define SH_FSM_WORK_CODE     0x6BD8 /* int in the machine: the code a state's Update hands it */
#define SH_FSM_LOOKUP_STOLEN 15     /* the lookup's detour window: three whole register saves */
#define SH_FSM_CLASS_CAP     96
#define SH_FSM_SALT_CAP      16

/* The stock class each kind is copied from (all four are used by stock decls). */
extern const char *const SH_FSM_SIBLINGS[SH_FSM_KINDS];
extern const char *const SH_FSM_KIND_NAMES[SH_FSM_KINDS];

/* One call into a mod's script. */
typedef struct sh_script_fsm_request {
    int         kind;        /* SH_FSM_* */
    const char *name;        /* the class as the script registered it */
    const char *hook;        /* a state's "enter", "update" or "exit"; NULL otherwise */
    int         index;       /* the node's parameter set */
    uint64_t    self;        /* the demon's script handle, 0 when the script does not know it */
    /* the answer */
    int         found;       /* the mod has that class (and hook) */
    int         truth;       /* a condition's answer */
    int         has_code;    /* a number was answered */
    int         code;        /* a transition's code, a state's code for its machine */
} sh_script_fsm_request;

/* What the bridge needs from the engine and the script runtime. */
typedef struct sh_fsm_ops {
    void    *(*lookup)(const char *name);            /* the engine's own lookup */
    int      (*on_game_thread)(void);
    uint64_t (*handle_of)(void *ai);                 /* the demon's script handle, 0 unknown */
    int      (*call)(const char *salt, sh_script_fsm_request *rq);   /* 1 answered */
    void     (*log)(const char *line);
} sh_fsm_ops;

void sh_fsm_set_ops(const sh_fsm_ops *ops);

/* `name` split into its parts: 1 when it is a script class name (kind, salt, class, n). */
int sh_fsm_parse(const char *name, int *kind, char *salt, size_t salt_cap, char *cls, size_t cls_cap, int *index);

/* The record for a script class name, made at its first lookup: NULL when the name is not
 * one, the game has no stock class to copy for its kind, or memory ran out. */
void *sh_fsm_class(const char *name);

/* The lookup's detour: the engine's own answer for every other name. */
void *sh_fsm_lookup(const char *name);

/* How many script classes the engine has asked for (tests, the status line). */
int sh_fsm_class_count(void);

/* Script runtime (script_runtime.c): run the mod's class. 1 when it answered; 0 when the mod
 * is not running, has no such class, or stopped on an error (its stop line says where). */
int sh_script_fsm_call(const char *salt, sh_script_fsm_request *rq);

#ifndef SH_SCRIPT_PURE
struct sig_result;
/* Resolve the lookup and detour it. Boot, before any mod decl is parsed. 1 installed. */
int sh_script_fsm_install(const uint8_t *module_base);
#endif

#endif
