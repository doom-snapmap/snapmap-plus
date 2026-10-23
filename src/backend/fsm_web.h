/* fsm_web.h -- per-configuration AI state-machine webs for package graph variants. */
#ifndef SH_FSM_WEB_H
#define SH_FSM_WEB_H
#include <stddef.h>
#include <stdint.h>
#include "signatures.h"

/* The engine builds each AI's state-machine web (Vulkan 0x108BFB0, OpenGL
 * 0x107DB20; one caller, at AI spawn) from every aifsmmanager declaration in its
 * resource list, keeping the graphs whose layer mask meets the AI's layer
 * configuration. Configurations are read only from list entry 0, name and type
 * lookups take the first matching graph, and state data are shared per
 * configuration name. An added declaration therefore cannot replace a stock
 * graph, and it enters every web whose configuration names one of its layers.
 *
 * This hook gives each build one coherent view of that list. A bound build
 * sees the stock list with each replaced declaration swapped at its own position
 * (entry 0 included, which carries the private configuration) and its own
 * additions kept. Every other issued product declaration reads as the
 * placeholder, whose graphs all have an empty layer mask and so meet no
 * configuration. Every other build sees the stock list with the same exclusions.
 *
 * Views are resolved and validated against the list's generation (its array
 * and count, and the issued set) and pinned per build. A table that does not
 * resolve completely is refused and the previous bindings stay. When the list
 * or the issued set changes, the next build resolves the current bindings
 * again; if that fails, the build is refused before the native builder runs
 * (the engine reports the AI as uninitialized) rather than given a mixed web.
 *
 * With ai_fsm_allowCache set (its default), the builder keeps each web it built
 * per interned configuration value for the life of the game object (reset at
 * map shutdown and by ai_reloadBehaviorFSMs) and replays it, reading list entry
 * 0 again for the configuration mask. The hook leaves that cache on and
 * redirects the replay's lookup too: a private configuration keys its own entry,
 * and its name is derived from issued names that are never reused. A refused
 * build never reaches the builder, which records the configuration before it
 * reads the list. */

/* The declaration name the product serves as the placeholder whenever anything
 * has been issued, independent of which packages are active. */
#define SH_FSM_WEB_PLACEHOLDER "smp/fsmweb_placeholder"

/* A build is bound when the behavior declaration that owns its configuration
 * (the builder receives behavior+0x558) is one of a binding's behaviors. The
 * builder then receives the binding's private configuration: its interned value
 * is read from the parsed configuration table of the declaration that replaces
 * SH_FSM_WEB_FIRST, so configuration lookup and the state-data factory use it.
 * Behavior text is never changed. */
#define SH_FSM_WEB_FIRST "states/alertcycle"

typedef struct sh_fsm_web_binding {
    const char *config;               /* private configuration, carried by the first replacement */
    const char *const *behaviors;     /* behavior declarations whose AIs build this web */
    size_t behavior_count;
    const char *const *from;          /* stock declaration names replaced in this web; must include SH_FSM_WEB_FIRST */
    const char *const *to;            /* replacements, parallel to from */
    size_t count;
    const char *const *extra;         /* product declarations kept at their own positions */
    size_t extra_count;
} sh_fsm_web_binding;

typedef struct sh_fsm_web_table {
    const sh_fsm_web_binding *bindings;
    size_t count;
    const char *const *owned;         /* product aifsmmanager declarations this table serves */
    size_t owned_count;
} sh_fsm_web_table;

/* Record product aifsmmanager declarations before they are registered with the
 * engine. Issued names stay excluded from every web but the ones that bind them
 * for the life of the process, whatever later publications do, because the
 * engine keeps a registered declaration in its list. 0 on allocation failure:
 * the caller must not register those declarations. */
int sh_fsm_web_issue(const char *const *names, size_t count);

/* Replace the bindings. The table's declarations (owned, replacements and
 * additions) are issued first. Returns 0, leaving the previous bindings active,
 * when the table is invalid or does not resolve against the list; NULL clears
 * the bindings. */
int sh_fsm_web_publish(const sh_fsm_web_table *table);
int sh_fsm_web_install(const uint8_t *module, const sig_result *results, size_t count);
int sh_fsm_web_installed(void);

#ifdef SH_FSM_WEB_TESTING
/* Drive the build wrapper and list resolver without a game. The synthetic
 * resolver and builder use the native calling convention. */
typedef void *(*sh_fsm_web_test_get)(void *list, int index);
typedef uintptr_t (*sh_fsm_web_test_builder)(void *manager, void *config);
void sh_fsm_web_test_bind(sh_fsm_web_test_get get, sh_fsm_web_test_builder builder, void *list);
uintptr_t sh_fsm_web_test_build(void *manager, void *config);
void *sh_fsm_web_test_decl_at(void *list, int index);
int sh_fsm_web_test_depth(void);
long sh_fsm_web_test_live(void);     /* specs and states still allocated */
void *sh_fsm_web_test_relay(int resolver);   /* installed tail jump: build, or list resolver */
int sh_fsm_web_test_uninstall(void);
void sh_fsm_web_test_reset(void);    /* drop every binding and issued name */
#endif
#endif
