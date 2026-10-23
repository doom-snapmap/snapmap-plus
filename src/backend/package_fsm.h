/* package_fsm.h -- compiler side of per-configuration state-machine webs (fsm_web.h). */
#ifndef SH_PACKAGE_FSM_H
#define SH_PACKAGE_FSM_H
#include <stddef.h>
#include <stdint.h>
#include "decl_tree.h"
#include "decl_entity_class.h"

/* A behavior names its state-machine web by layer configuration
 * (edit.aiFSMManagerDeclLayersConfig); the engine resolves that name only in
 * states/alertcycle, the first aifsmmanager declaration. A bound web gets a
 * private configuration listing exactly its base configuration's layers, so the
 * native mask is unchanged and only the web's declarations differ. */
#define SH_PACKAGE_FSM_FIRST "states/alertcycle"
#define SH_PACKAGE_FSM_PLACEHOLDER_SOURCE "states/behavior_generic"

typedef struct sh_package_fsm_config {
    const char *name;     /* private configuration */
    const char *base;     /* configuration whose layers it lists */
} sh_package_fsm_config;

/* The layers one configuration names in a layer-configuration declaration, in
 * authored order. 1 found, 0 absent, -1 malformed or allocation failure. */
int sh_package_fsm_config_layers(sh_decl_source first, const char *config,
    char ***layers, size_t *count, char *error, size_t capacity);
void sh_package_fsm_names_free(char **names, size_t count);

/* The mask one configuration selects, in the first declaration's own layer
 * order: the bits the engine tests every graph's mask against. 1 found, 0
 * absent, -1 malformed (a configuration layer the declaration does not list,
 * say). */
int sh_package_fsm_config_mask(sh_decl_source first, const char *config, uint32_t *mask,
    char *error, size_t capacity);
/* The union of a state-machine declaration's graph masks in its own layer
 * order. 1 with at least one graph, 0 none, -1 malformed. */
int sh_package_fsm_graph_mask(sh_decl_source declaration, uint32_t *mask, char *error, size_t capacity);

/* A copy of the layer-configuration declaration with each private
 * configuration added after the native ones. Every base must exist and every
 * name must be new and a plain identifier. NULL with a reason otherwise. */
char *sh_package_fsm_first(sh_decl_source first, const sh_package_fsm_config *configs,
    size_t count, size_t *length, char *error, size_t capacity);

/* The placeholder: a copy of a stock state-machine declaration whose every
 * graph has an empty layer set, so it meets no configuration. */
char *sh_package_fsm_placeholder(sh_decl_source stock, size_t *length, char *error, size_t capacity);

/* Deterministic private configuration: "smp_" and 32 hex digits of SHA-256
 * over the base configuration, each replaced declaration with its replacement
 * (sorted by replaced name) and the additions (sorted). Independent of the
 * order entries are given in. */
int sh_package_fsm_config_name(const char *base, const char *const *from, const char *const *to,
    size_t count, const char *const *extra, size_t extra_count, char out[40]);

/* ---- Selection (the source link's behavior edges) ------------------------ */

/* The configuration a behavior's expanded state names (inherited fields
 * included, parents read through source): 1 with a malloc-owned name, 0 none,
 * -1 unresolvable with a reason. */
int sh_package_fsm_behavior_config(sh_decl_source behavior, const sh_decl_entity_class_source *parents,
    char **config, char *error, size_t capacity);

/* Layer masks as one scope resolves its declarations. config NULL asks for a
 * declaration's graph mask (sh_package_fsm_graph_mask), otherwise for that
 * configuration's mask in the declaration (sh_package_fsm_config_mask). Same
 * results: 1 found, 0 absent (or no such declaration), -1 failed. */
typedef struct sh_package_fsm_layers {
    void *context;
    int (*exists)(void *context, const char *declaration);   /* 1, 0 absent, -1 failed */
    int (*mask)(void *context, const char *declaration, const char *config, uint32_t *mask);
} sh_package_fsm_layers;

/* Visit every state-machine declaration a configuration's AIs build from:
 * first SH_PACKAGE_FSM_FIRST, which the engine reads the configuration from
 * whatever its own graphs contribute (also when it lacks the configuration),
 * then each of names whose graph mask meets the configuration's mask, once.
 * Returns 1, or 0 when a mask failed or visit returned 0. An absent first
 * declaration selects nothing. */
int sh_package_fsm_select(const char *config, const sh_package_fsm_layers *layers,
    const char *const *names, size_t count, int (*visit)(void *context, const char *name), void *context);

/* ---- Webs (the compiler's table) ----------------------------------------- */

#define SH_PACKAGE_FSM_PLACEHOLDER "smp/fsmweb_placeholder"   /* fsm_web.h: SH_FSM_WEB_PLACEHOLDER */
#define SH_PACKAGE_FSM_FIRST_PREFIX "smp/fsmweb/"             /* + private configuration */

/* One linked behavior whose state-machine bindings survived (changed
 * declarations its configuration selects in its scope). */
typedef struct sh_package_fsm_bound {
    const char *behavior;          /* its linked name */
    const char *config;            /* the configuration it names */
    sh_decl_source first;          /* its scope's first declaration, as linked */
    const char *const *from;       /* changed stock declarations it binds (the first one too, if changed) */
    const char *const *to;         /* their linked names, parallel */
    size_t count;
    const char *const *extra;      /* linked names of declarations its scope adds */
    size_t extra_count;
} sh_package_fsm_bound;

/* One web: the bound behaviors that build it, its private configuration, its
 * replacements (from[0] is SH_PACKAGE_FSM_FIRST, to[0] its generated first
 * declaration carrying the private configuration) and its additions. */
typedef struct sh_package_fsm_web {
    char *config;
    char **behaviors;
    size_t behavior_count;
    char **from, **to;
    size_t count;
    char **extra;
    size_t extra_count;
} sh_package_fsm_web;
/* Declarations the compiler generates: each web's first declaration. */
typedef struct sh_package_fsm_declaration { char *name, *text; size_t length; } sh_package_fsm_declaration;
typedef struct sh_package_fsm_webs {
    sh_package_fsm_web *webs;      /* sorted by private configuration */
    size_t count;
    char **owned;                  /* every product state-machine declaration served, sorted */
    size_t owned_count;
    sh_package_fsm_declaration *generated;
    size_t generated_count;
} sh_package_fsm_webs;

/* Build the webs of one link. Behaviors with the same configuration, first
 * declaration and substitutions share one web; the private configuration name
 * is sh_package_fsm_config_name over them (the changed first declaration
 * included), so equal webs dedupe across compilations and different ones never
 * meet. owned lists the link's other product state-machine declarations; the
 * generated first declarations and the placeholder are added. A behavior
 * whose configuration its first declaration lacks is refused. NULL with a
 * reason on failure; an empty table when nothing binds or is owned. */
sh_package_fsm_webs *sh_package_fsm_webs_build(const sh_package_fsm_bound *bound, size_t count,
    const char *const *owned, size_t owned_count, char *error, size_t capacity);
/* Map webs before local ones; equal private configurations are the same web
 * (their behaviors unite) and a behavior binds one web. Owned names unite. */
sh_package_fsm_webs *sh_package_fsm_webs_join(const sh_package_fsm_webs *map, const sh_package_fsm_webs *local,
    char *error, size_t capacity);
/* The webs keep accepts a behavior of, holding only those behaviors; owned
 * names and generated declarations stay whole. */
sh_package_fsm_webs *sh_package_fsm_webs_filter(const sh_package_fsm_webs *webs,
    int (*keep)(void *context, const char *behavior), void *context, char *error, size_t capacity);
void sh_package_fsm_webs_free(sh_package_fsm_webs *webs);
#endif
