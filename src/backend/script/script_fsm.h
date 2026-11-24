/* script_fsm.h -- classes a mod's script supplies to the game's own AI state machines.
 *
 * The engine looks up each aifsmmanager class by name (VK 0x141583B20 / GL 0x141584BB0); the detour answers
 * smp.<kind>.<salt>.<class>.<n> (n: the node's parameter set) with a copy of a stock class of that kind whose one
 * virtual calls the mod's script. Scripts run on the game's thread only; elsewhere a class answers "no", no code,
 * or nothing, and logs a line. */
#ifndef SH_SCRIPT_FSM_H
#define SH_SCRIPT_FSM_H

#include <stddef.h>
#include <stdint.h>

enum { SH_FSM_CONDITION, SH_FSM_SNIPPET, SH_FSM_TRANSITION, SH_FSM_STATE, SH_FSM_KINDS };

/* Engine facts (build 13954591): a type record is 0x90 bytes with name at +0x00, create +0x28, clone +0x30,
 * placement constructor +0x38. A condition and a snippet implement vtable slot 2, a transition slot 12 (fills a
 * code at out+8), a state slots 25 OnEnter, 26 OnExit, 28 Update; a state hands its machine a code at fsm+0x6BD8. */
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
