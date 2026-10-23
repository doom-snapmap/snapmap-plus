/* Native map representation of package entity definitions (R137).
 *
 * A map entity whose inline entityDef inherits a product-served definition P
 * can often be saved as native SnapMap data instead: inherit the first native,
 * unchanged ancestor S and carry P's own edits as inline state. Each verified
 * site is decided on its own, with its complete dependency graph; another use
 * of P may keep its package identity. Only exact equivalents convert:
 * the combined inline layer must expand to the same state as P's chain plus the
 * instance's own values, every value must survive SnapMap's carrier, and the
 * site must not depend on P's lookup identity. Everything else keeps its
 * package route, with the reason and whether it is a native limit. */
#ifndef SH_PACKAGE_NATIVE_INLINE_H
#define SH_PACKAGE_NATIVE_INLINE_H

#include "decl_dependencies.h"
#include "decl_native_carrier.h"

/* Outcome of native value evaluation. Source-tree inequality is not native
 * inequivalence, so these never classify a site as an ENGINE limit. */
typedef enum sh_native_eval_status {
    SH_NATIVE_EVAL_UNAVAILABLE = 0, /* no evaluator, or it declined to run */
    SH_NATIVE_EVAL_EQUAL,           /* native states are equal within its coverage */
    SH_NATIVE_EVAL_UNEQUAL,         /* this candidate changes native state */
    SH_NATIVE_EVAL_UNSUPPORTED,     /* a value lies outside the recovered native coverage */
    SH_NATIVE_EVAL_FAILED           /* native parse, read or write failed */
} sh_native_eval_status;

/* One definition of a target's inheritance chain prepared for a private
 * evaluation with its authored text: one the engine cannot resolve by name
 * with the document's source, or one beneath such a definition. Chains list
 * them nearest to the instance first. */
typedef struct sh_native_eval_layer {
    const char *name;
    const char *text;
} sh_native_eval_layer;

/* A locally proved equivalence the converter hands the evaluator (R-212):
 * reduce rewrites one prepared state text, removing only the earlier
 * occurrences of a repeated member whose last occurrence is a complete
 * assignment by its selected reader and whose removal is proved by
 * sh_native_inline_reduce's rules. The evaluator applies it identically to
 * the target's and the candidate's prepared texts and compares the results
 * exactly; class, instantiable and member checks still apply. Returns 1 with
 * malloc-owned canonical text (removed counts the occurrences dropped), 0
 * when the text is outside the proof (reason set), -1 on failure. */
typedef struct sh_native_eval_reduction {
    void *context;
    int (*reduce)(void *context, const char *state, size_t length, char **reduced, size_t *reduced_length,
        size_t *removed, char *reason, size_t capacity);
} sh_native_eval_reduction;

/* Another typed map site bound to a placed entity's definition: owner
 * "entity/N" or "world" (NULL or empty for a historical binding without
 * sites), reference its typed value path (map_binding_sites.h), such as
 * "/k:entityDef/k:state/k:edit/k:spawnerEntityPair/k:entityStatic". */
typedef struct sh_native_inline_use { const char *owner, *reference; } sh_native_inline_use;

typedef struct sh_native_inline_hint_selector {
    const char *row, *entity; /* actual editor row and its entityDef identity */
    int category;           /* actual row's moduleAutomation value */
} sh_native_inline_hint_selector;
typedef struct sh_native_inline_hint_bank {
    const char *name;       /* actual loaded element 0, never a default-name substitute */
    sh_native_inline_hint_selector selectors[5]; /* large, medium, small, powerup, weapon */
} sh_native_inline_hint_bank;

typedef struct sh_native_inline_source {
    void *context;
    /* Effective entityDef text in this map's authored context. Returns 1 with
     * malloc-owned text and served=1 when the product serves that identity
     * (package-private, or a stock name a package changes), served=0 for the
     * unchanged native original; 0 absent; -1 unavailable. A served definition
     * that replaces a stock identity under a private name also reports that
     * stock name through replaces (may be NULL), malloc-owned. */
    int (*read)(void *context, const char *name, sh_decl_source *text, int *served, char **replaces);
    /* Native class ancestry: 1 derives, 0 unrelated, -1 unavailable. */
    int (*derives)(void *context, const char *class_name, const char *base_name);
    /* Reflected field schema for typed checks of converted state. Required. */
    const sh_decl_dependency_schema *schema;
    /* Other map uses of a definition, each decided by its own consumer: name
     * is the one this entity inherits, as its document spells it, or a served
     * ancestor its conversion folds. Lists every
     * typed site other than this entity's own inherit that is bound to that
     * definition; another entity's inherit is an independent occurrence and
     * is not listed. Writes at most capacity and returns how many exist,
     * or -1 when unavailable. The strings stay valid for the document call.
     * NULL lists none. */
    int (*looked_up)(void *context, size_t entity, const char *name, sh_native_inline_use *uses, size_t capacity);
    /* Caller exclusion: return a static reason to keep this entity's package
     * route (and set *limit: ENGINE when native map data cannot carry what the
     * package still does, such as a global stock change), or NULL to allow
     * conversion. May be NULL. */
    const char *(*keep)(void *context, size_t entity, const char *name, sh_decl_carrier_limit *limit);
    /* Current native entity carrier version. Supported original versions are
     * upgraded on a derived copy before source lookup and inheritance folding;
     * only an accepted equivalent result is emitted at this version. */
    int version;
    /* Native placement. SnapMap's map validator admits an entity only when its
     * inherit names the entityDef of a palette row (a snapEditorEntityDef).
     * Return NULL when base, the native ancestor the site would inherit, has a
     * shipped row and any editor row of the site's definition is a plain copy
     * of it; otherwise a reason (valid until the next call) with *limit. May
     * be NULL only where no map validation applies. */
    const char *(*placement)(void *context, const char *from, const char *base, sh_decl_carrier_limit *limit);
    /* Native value evaluation of every conversion: compare the native state
     * of the target definition (inherit P plus the instance values) with the
     * candidate (inherit base plus the folded layer and the instance). Only
     * EQUAL converts; every other status keeps the package as a GAP with a
     * distinct reason. NULL means no native evaluator is bound. reduction is
     * non-NULL only when the candidate drops repeated complete assignments the
     * target's first stateful layer keeps; it must be applied to both prepared
     * texts, never used to skip the comparison. */
    sh_native_eval_status (*evaluate)(void *context, size_t entity, const char *from, const char *base,
        const sh_decl_node *target, const sh_decl_node *candidate, const sh_native_eval_reduction *reduction,
        char *detail, size_t capacity);
    /* Effective text of a declaration of another type, by its declaration
     * folder (such as "aipoolnumbers"), with read's results and served flag.
     * The world's AI spawn pool is read through it: SnapMap's play world
     * inherits worldspawn, whose aiSpawnPoolDecl names an aiPoolNumbers
     * declaration. When a pool is named and this is NULL or unavailable, every
     * use stays a product gap with that reason. */
    int (*read_decl)(void *context, const char *type, const char *name, sh_decl_source *text, int *served);
    /* The typed declaration names the map's sites hold, one per index: 1 with
     * borrowed family (declaration folder, such as "entitydef") and name, 0
     * past the end, -1 when the walk did not complete. A placed copy's play
     * declaration is named from its inherit, so an entityDef name that can be
     * that generated name ties its consumer to this occurrence. NULL states
     * that no site holds a declaration name. */
    int (*typed_name)(void *context, size_t index, const char **family, const char **name);
    /* The map-local play declaration uses the map binder bound to exactly one
     * placed occurrence (sh_package_map_local_use), one per index: 1 with the
     * typed site (owner "entity/N", typed value path) and that occurrence's
     * generated name as this document spells it, 0 past the end, -1 when the
     * walk did not complete. typed_name lists none of them. When the occurrence
     * converts, these sites, and no other bytes, are rewritten to the renamed
     * copy. NULL states that no site holds one. */
    int (*local_use)(void *context, size_t index, const char **owner, const char **reference, const char **name);
    /* Every declaration the product serves in this map's context, one per
     * index: 1 with family (declaration folder) and name valid until the next
     * call, or both NULL for an index that is not one; 0 past the end; -1 when
     * the served set is not known. Served declarations are not map sites, so
     * looked_up never lists a consumer inside one. NULL lists none (test
     * doubles without a served library). */
    int (*served_decl)(void *context, size_t index, const char **family, const char **name);
    /* The map's typed uses of served definitions, one per index: 1 with the
     * use's site (owner "entity/N", typed value path), and, unless the site
     * is an entity's own inherit, name as this document spells it and kept:
     * a static reason to keep that use's package route (as keep decides for
     * its own source contexts, with *limit) or NULL. name is NULL for an
     * index that is not such a use. 0 past the end; -1 unavailable. Each
     * independent use is decided at its own site (R-293). NULL lists none. */
    int (*bound_use)(void *context, size_t index, sh_native_inline_use *use, const char **name, const char **kept,
        sh_decl_carrier_limit *limit);
    /* The family a typed declaration pointer of class_name resolves name in,
     * as the native pointer reader does in this map's effective source: its
     * registered managers in native search order, the first whose source has
     * the name. 1 with family (declaration folder, valid until the next call),
     * 0 when no candidate family has it, -1 unavailable. NULL resolves none,
     * so every reference it would decide is not established. */
    int (*resolve_decl)(void *context, const char *class_name, const char *name, const char **family);
    /* The reflected schema schema views, for the reader its field view does
     * not express: the graph envelope (sh_decl_graph_dependencies_with), whose
     * records select their own reflected classes. NULL leaves a graph
     * declaration's state unread, so a census reaching one is incomplete. */
    struct sh_decl_native_schema *native_schema;
    /* 1 only when the current native registry, supported receiver dispatch and
     * reflected timeline-component execution prove this name event has no
     * effect beyond its discarded result. Other returns keep the existing
     * consumer obligation. Used only at reflected eventCall sites; direct
     * result-consuming callers do not acquire this proof. */
    int (*discarded_name_event)(void *context, const char *event_name);
    /* 1 only when the verified event handler constructs the declaration
     * argument by name and never selects an existing instance by its
     * declaration identity. Called only for a reflected event decl argument.
     * argument_count is authored args.num. argument_signature is borrowed for
     * this call and indexed by native argument number: reader format letters,
     * '0' explicit NULL, '?' omitted; it ends after the highest supplied index.
     * Omitted arguments retain existing/default state. Unknown metadata returns
     * no proof; a partial block need not state the full native arity. */
    int (*event_decl_constructs)(void *context, const char *event_name, size_t argument_index, size_t argument_count,
        const char *argument_signature, const char *decl_class);
    /* Effective raw asset text, from the same immutable compiled/original
     * source and delivery view as read(). 1 owned text, 0 absent, -1 incomplete.
     * Module names are actual declaration paths; no generated/decls prefix is
     * invented for them. */
    int (*read_asset)(void *context, const char *path, sh_decl_source *text, int *served);
    /* Current loaded bank/row associations at this save's stable native
     * boundary. Borrowed until the source view ends. Every call revalidates
     * the same cohort, including the five actual row receivers, definitions,
     * categories and registry membership; -1 means unavailable or changed. */
    int (*hint_bank)(void *context, const sh_native_inline_hint_bank **bank);
    /* Actual palette receiver lower_bound plus published extension rows. 1
     * selects a borrowed stable row, 0 is a positive native miss, -1 unknown.
     * Every call must revalidate the same capture generation. */
    int (*hint_row)(void *context, const char *key, const sh_native_inline_hint_selector **row);
} sh_native_inline_source;

typedef struct sh_native_inline_site {
    size_t entity;              /* index in the document's entities array */
    char *from, *to;            /* P, and S when converted */
    int converted;
    sh_decl_carrier_limit limit; /* ENGINE for a native limit, GAP otherwise */
    char reason[2048];          /* names several private identities in full */
    size_t renamed;             /* map-local uses rewritten to the converted copy's name */
    /* Placed: ancestry filters naming a definition this conversion folds
     * whose exact match set is this occurrence alone; each is rewritten to the
     * converted copy's name. Independent: the exact-identity operands naming
     * from (name, as the map spells it) that its group renames to to. Owned
     * strings, freed with the report. */
    struct sh_native_inline_retarget { char *owner, *reference, *name; } *retargets;
    size_t retargeted;
    /* An independent typed use (bound_use) decided at its own site: owner
     * and typed value path, owned; NULL for a placed occurrence. Converted,
     * the site names to, the first native ancestor of from. */
    char *owner, *reference;
    /* Converted only under the plan's holder substitution: sigma, typed
     * references of from (for a placed occurrence, of its chain's layers)
     * name other served definitions the plan converts with it; carried
     * (independent only), count observers of from that sit in served holders
     * converting to their native ancestors with it. */
    int sigma;
    size_t carried;
    /* Kept by the selected plan, not by missing support or a native limit:
     * uses that must convert together or not at all, and a use whose earlier
     * round relied on an entry a later round withdrew. Reported as a plan
     * conflict; limit stays GAP. */
    int conflict;
} sh_native_inline_site;

/* One entry of the plan's holder substitution: a served entity definition
 * and its first native ancestor, or (drop set) a served loot drop and the
 * native loot drop whose object it builds under the plan. */
typedef struct sh_native_inline_substitution { char *from, *to; int drop; } sh_native_inline_substitution;
typedef struct sh_native_inline_report {
    sh_native_inline_site *sites;
    size_t count;
    /* The plan's final holder substitution (served definitions and their
     * native ancestors, served loot drops and the native drops they stand
     * for), owned; empty unless a converted site relies on it. */
    sh_native_inline_substitution *substitutions;
    size_t substitution_count;
    /* How the plan's loot drop entries were decided (a census that could not
     * be built or is incomplete, each served loot drop play reaches and
     * whether it is an entry, entries a round withdrew), one per line, owned;
     * NULL when the product serves no loot drop. Diagnostic only. */
    char *notes;
} sh_native_inline_report;
void sh_native_inline_report_free(sh_native_inline_report *report);
/* Withdraws every conversion report lists, as a plan that is not taken: each
 * converted site keeps its package with limit and reason, and no success
 * annotation stays (its copy's name, holder substitution, carried observers,
 * retargets, followed map-local uses); the plan's holder substitution goes
 * with them. Sites already kept are unchanged. */
void sh_native_inline_withdraw(sh_native_inline_report *report, sh_decl_carrier_limit limit, const char *reason);

/* Rewrite eligible entityDef values of one native map document, and the
 * independent typed uses that can name a native definition instead. Entities
 * whose inherit is not product-served are untouched and unreported; an
 * independent use is reported unless it is not a traced construction or its
 * definition changes state. Bytes outside rewritten values stay exact. Returns malloc-owned JSON (a copy when
 * nothing converts) and fills report; NULL with error on operational failure
 * (report then stays empty). This never edits sources or the live editor map. */
char *sh_native_inline_document(const char *json, size_t length,
    const sh_native_inline_source *source, size_t *output_length,
    sh_native_inline_report *report, char *error, size_t capacity);
/* The count observations plan's holder substitution was decided on, read
 * again from a derived document as its own sources serve it: every evaluated
 * count observer of input (as source reads it) that involves the substitution
 * is one of derived (as derived_source reads it) with the substitution
 * applied; derived has no other such observer and reaches no substituted
 * definition. 1 equal (also when plan has no substitution), 0 different
 * (reason says how), -1 failure. */
int sh_native_inline_observations(const sh_native_inline_source *source, const char *input, size_t input_length,
    const sh_native_inline_report *plan, const sh_native_inline_source *derived_source, const char *derived,
    size_t derived_length, char *reason, size_t capacity);

/* The play declaration SnapMap registers for each placed entity. The play map
 * builder (VK 1404ee8e0 / GL 1404ee2b0) gives every instanceEntities member a
 * copy named "%u_%s/%s_%u" (group index, module name, its inherit, uniqueId)
 * and registers it in the entityDef list before any entity spawns
 * (MapEntityForSnapEntity VK 1404f1350 / GL 1404f0c50). One row per entity:
 * module_index is NULL when instanceEntities does not place the uniqueId in
 * exactly one group, module_name NULL when that group's loaded module name is
 * unknown, inherit NULL when the entity states no plain inherit. */
typedef struct sh_native_inline_generated {
    size_t entity;
    char *unique, *module_index, *module_name, *inherit;
} sh_native_inline_generated;
typedef struct sh_native_inline_generated_set { sh_native_inline_generated *rows; size_t count; } sh_native_inline_generated_set;
/* 1 read (possibly no rows), 0 not a native map document, -1 allocation. */
int sh_native_inline_generated_read(const char *json, size_t length, sh_native_inline_generated_set *out);
void sh_native_inline_generated_free(sh_native_inline_generated_set *set);
/* The row's play declaration name under inherit (malloc-owned); NULL when a
 * part is unknown or on allocation failure. */
char *sh_native_inline_generated_name(const sh_native_inline_generated *row, const char *inherit);

/* Proved complete-assignment reduction of one entity state block under
 * class_name (R-212), in place: an earlier occurrence of a repeated member is
 * removed only when its last occurrence is a complete assignment (a plain
 * scalar or its reader's ASSIGN token form), every earlier one is such an
 * assignment or a native no-op, nothing between them can read that storage
 * (only such pure writes, natively ignored members or blocks of them), the
 * name is not one the inheritance post-pass or list readers count (num and
 * item prefixes), and no removed occurrence is an instance value. Adjacent
 * occurrences of a block with no registered reader are concatenated, which
 * keeps every read in order. 1 done (removed counts dropped occurrences; no
 * repeat remains), 0 outside the proof (reason set), -1 failure. */
int sh_native_inline_reduce(const sh_decl_dependency_schema *schema, sh_decl_node *edit, const char *class_name,
    size_t *removed, char *reason, size_t capacity);

/* Representation work since the last reset (reset nonzero clears it):
 * documents converted or examined, their placed entities, the sites decided,
 * loot census builds, the largest census in nodes, node pushes, and the nodes
 * those pushes compared. Diagnostics for a caller's timing line. */
typedef struct sh_native_inline_stats {
    size_t documents, placed, sites, census_builds, census_nodes, census_pushes, census_compared;
} sh_native_inline_stats;
void sh_native_inline_stats_take(sh_native_inline_stats *out, int reset);

#ifdef SH_NATIVE_INLINE_TESTING
/* Test the original-version policy without stamping an accepted version or
 * converting inheritance. Return 1/copy, 0/coverage, or -1/allocation failure. */
int sh_native_inline_test_version_copy(const char *json, int target, char **copy);
void sh_native_inline_test_version_alloc_fail_after(long count);
/* Exercise the drop object's typed equivalence relation without constructing
 * a map or a native object. Substitutions are borrowed for this call. */
int sh_native_inline_test_lootdrop(const sh_native_inline_source *source, const char *holder,
    const sh_native_inline_substitution *substitutions, size_t count, char *native, size_t native_capacity,
    char *reason, size_t capacity);
/* The loot census's work since the last reset (reset nonzero clears it):
 * builds started, node pushes, and nodes those pushes compared. */
void sh_native_inline_test_census_counts(size_t *builds, size_t *pushes, size_t *compared, int reset);
#endif

#endif
