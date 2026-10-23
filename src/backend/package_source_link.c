#include "package_source_link.h"
#include "package_native_names.h"
#include "package_keys.h"
#include "decl_entity_class.h"
#include "package_virtual_material.h"
#include "package_font.h"
#include "package_movie.h"
#include "package_audio_program.h"
#include "package_fsm.h"
#include "decl_native_rebind.h"
#include "config_json.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <intrin.h>

typedef struct sl_shader_program {
    char *name;
    size_t parent;
    uint64_t mask;
    sh_package_shader_program decoded;
} sl_shader_program;
typedef struct sl_shader_file {
    const sh_compiled_resource *file;
    size_t program;
    sh_package_shader_path_kind kind;
} sl_shader_file;
/* conditional: a state-machine selection (sh_package_link_unit.conditional). */
typedef struct sl_implicit { char *slot; struct sl_unit *target; int conditional; } sl_implicit;
/* One reference site discovery bound, in bind order (see sl_reference). */
typedef struct sl_site { const sh_compiled_resource *file; char *slot; struct sl_unit *target; } sl_site;
/* After freezing: the maximal run of a unit's sites one file bound, ending before end (sl_freeze_sites). A unit's
 * runs cover its sites in order, so its last run ends at its site_count. */
typedef struct sl_site_run { const sh_compiled_resource *file; size_t end; } sl_site_run;
/* Capture-owned blocks for lists that grow while units are discovered (sl_arena_grow). free[c]: regions of at least
 * 2^c items that no list holds any more (a list that moved, a freed unit's), reused before the newest block. */
typedef struct sl_block { struct sl_block *next; size_t size, used, reserved; } sl_block;
typedef struct sl_arena { sl_block *blocks; size_t block_size; void *free[32]; } sl_arena;
/* A unit's discovery state, read only until the inputs freeze. It lives in the capture's discovery arena from the
 * unit's creation until the freeze returns that arena whole (sl_discovery_release); sl_unit.discovery is NULL from
 * then on. A complete library captures about a million units, so the state no frozen reader needs is not carried
 * into the contribution phase, the identity passes and the link (R-341). */
typedef struct sl_discovery {
    sh_package_link_unit record; /* the unit's link record until the freeze writes it into the unit table */
    /* In the capture's dependency arena. */
    struct sl_unit **dependencies;
    sl_implicit *implicit; /* owned; frozen as sl_unit.implicit_edges */
    struct sl_unit *parent; /* Unit whose discovery first requested it; NULL for seeds. */
    char *seed; /* owned: why a unit without a same-scope parent entered the capture. */
    /* Sites each discovered file bound, in the capture's site arena; the freeze keeps them as
     * sl_unit.site_edges with each file's run. */
    sl_site *sites;
    /* The semantics an image's private copy would carry when the availability callback marked it (availability);
     * owned, NULL otherwise. */
    unsigned char *availability_semantics;
    /* Counts and capacities fit 32 bits (R-343): a list refuses to grow past UINT32_MAX entries. capacity: room in
     * unit.files; reverse_gap (observation only): 1 + index of its accounted reader gap; seeded_witnesses: its witness
     * count when seeded. */
    uint32_t capacity, dependency_count, dependency_capacity, implicit_count, reverse_gap, seeded_witnesses, site_capacity;
    /* Shared stock (sl_fold_scope): 1 + its position in the batch being folded, 0 outside one. */
    uint32_t fold_slot;
    /* Unchanged-scope reuse, recording (R-348): 1 + the set of paths its discovery consulted (sl_reuse_set_of), when
     * its recording was replayed or kept and read only captured originals; 0 otherwise. */
    uint32_t consulted;
    /* stock_inputs: its discovery read only captured originals, through paths the baseline does not supply either
     *   (sl_stock_inputs), so the baseline scope would discover it alike.
     * material_members: its material companions were requested.
     * fixed_reached: a fixed native ABI unit of its own scope binds it, directly or through other bindings
     *   (sl_fixed_reach); such a unit keeps its original binding and so does all it binds.
     * availability: the availability callback's answer for an image (1: exclusion placeholder in a gate-opening scope,
     *   or one state its scope authored names; sl_availability).
     * authored_image: an image the scope supplies, or one a supplied unit of the same scope binds (sl_availability).
     * nominated: seeded as a stock root affected by its scope's changes.
     * discovered_done: its discovery completed (sl_discover_unit).
     * movie_captured: constructor requests of a cooked movie were enumerated.
     * product: its original is the product's own output (sl_product).
     * seeded: created by seeding (the scope's supplied files); reverse nomination reads seeded units only, as seeded.
     * preparation_checked: its native preparation was checked.
     * native_requests: the native implicit adapter requested something for it (sl_native_request). */
    unsigned material_members : 1, fixed_reached : 1, availability : 1, nominated : 1, discovered_done : 1,
        movie_captured : 1, product : 1, seeded : 1, preparation_checked : 1, stock_inputs : 1, native_requests : 1,
        authored_image : 1;
} sl_discovery;
/* A unit's rarely used capture state, apart from the million records of a complete library (R-343): allocated by
 * its first writer (sl_extra_of), read through SL_EXTRA, freed with the unit. */
typedef struct sl_extra {
    sl_shader_program *programs;
    size_t program_count;
    sl_shader_file *shader_files;
    size_t shader_file_count;
    char *fsm_config; /* The layer configuration an aibehavior names (sl_fsm_edges). */
    sh_package_link_file_identity *identities; /* Owned per-file identities (movie pairs). */
    sh_compiled_resource *prepared; /* Borrowed metadata; owns only body. NULL unless prepared (sl_prepared_set). */
    const sh_compiled_resource *unprepared; /* Exact original certificate input. */
    /* Diagnostics: why this unit's own binding is not certified original. */
    char *original_note;
} sl_extra;
typedef struct sl_unit {
    /* The unit's link record (R-343): its discovery record until the freeze, then its row of the capture's unit table,
     * or the capture's own copy of that row once the table's borrower edits it (preserved). */
    sh_package_link_unit *unit;
    /* A capture holds fewer than UINT32_MAX units, scopes and sites or discovered files per unit (sl_find,
     * sl_reference, sl_discovered_add refuse more). */
    uint32_t scope, index;
    /* The sites discovery bound (sl_discovery.sites), and the files it discovered; after freezing, the same sites
     * as (slot, unit index) edges with each file's run (sh_package_source_link_sites): site_edges and site_runs
     * point into the capture's frozen blocks until it closes. */
    uint32_t site_count, discovered_count;
    /* authored_request: state its scope authored names this unit (sl_authored);
     * authored_layer: that state inherits it, so its own state is authored too.
     * campaign_boundary: only the campaign archive carries it and no authored
     * state names it, so it is the stock route's own absence (sl_files).
     * presentation: a catalog card changed only through its presentation (sl_presentation_slot).
     * replayed: discovery replayed another scope's recorded reads (sl_replay_apply).
     * coverage, derived_coverage: its captured text, and its last derived text, bound with undecided
     * native semantics (sh_package_source_binding.coverage).
     * Shared stock (sl_fold_scope): shared_stock, a baseline unit some package scope binds instead of its own copy;
     * stock_only, such a unit no baseline unit reaches (not a member of the baseline scope; sl_promote);
     * fold_certified, certified when it became shared (the certification loop keeps that certificate). */
    unsigned authored_request : 1, authored_layer : 1, campaign_boundary : 1, presentation : 1, replayed : 1,
        coverage : 1, derived_coverage : 1, preserved : 1, shared_stock : 1, stock_only : 1, fold_certified : 1;
    sh_package_link_edge *implicit_edges; /* unit.implicit, then unit.conditional; owned */
    sl_extra *extra;
    const sh_compiled_resource **discovered;
    sh_package_link_edge *site_edges;
    const sl_site_run *site_runs;
    sl_discovery *discovery; /* until the freeze */
} sl_unit;
typedef struct sl_fsm_mask {
    const sh_compiled_resource *file; /* the declaration's source in some scope */
    char *config; /* NULL: the union of its graph masks; else this configuration's */
    uint32_t mask;
    int status; /* 1 found, 0 absent (no graphs, or no such configuration) */
} sl_fsm_mask;
typedef struct sl_original {
    sh_compiled_resource resource;
    int borrowed; /* resource.body is borrowed (native.original_view), never freed here */
    int status;
    int identified; /* identity holds the payload identity of resource.body */
    int released;   /* reverse-only owned bytes dropped; reread must match identity */
    sh_package_file_identity identity;
    int producing;  /* its product original is being generated */
    unsigned char mark; /* the capture's lookups of its path (sl_lookup_marked) */
} sl_original;
/* kind 1: exact canonical supplied-path lookup (including a true miss).
 * kind 2: successful identity of one immutable supplied resource. No bytes or
 * provider references are retained, and the two key domains never alias. */
typedef struct sl_lookup_entry {
    uint64_t hash;
    size_t scope;
    char *key;
    const sh_compiled_resource *file;
    sh_package_file_identity identity;
    int kind;
} sl_lookup_entry;
typedef struct sl_lookup_cache {
    sl_lookup_entry *slots;
    size_t count, capacity, bytes, peak, limit;
} sl_lookup_cache;
/* One recorded discovery a later discovery may replay (sl_replay_*): the
 * reads a scope's reader made to prepare, bind and request a unit, and what
 * they read. Each file, and each scoped path the reader consulted, keeps how
 * it resolved: supplied by the scope (state -1) or the capture's original
 * (its status; 0 is absent), with its content identity when a memo keeps the
 * recording. valid is cleared when recording ran out of memory. */
typedef struct sl_replay_ref { char *slot, *family, *name; int request; } sl_replay_ref;
/* mark: the capture's canonicalizer (sl_lookup_marked) under which path is its own lookup key, 0 none known. */
typedef struct sl_replay_input { char *path; int state; unsigned char mark; sh_package_file_identity identity; } sl_replay_input;
typedef struct sl_replay {
    struct sl_replay *next; /* other recordings of this family and name */
    uint64_t serial; /* unique in the process: a freed recording's address may be reused */
    char *family, *name;
    unsigned long long capture; /* the capture that recorded it, and its scope */
    size_t scope;
    sl_replay_input *files;
    size_t file_count;
    unsigned char *prepared; /* the native preparation output, when prepared */
    size_t prepared_length;
    /* coverage: a binding left the unit's native semantics undecided (sl_bind_member), as a replay must again. */
    int was_prepared, valid, coverage;
    unsigned long long marked; /* the capture whose lookups set its inputs' marks (sl_lookup_input) */
    sl_replay_input *lookups; /* every scoped path consulted */
    size_t lookup_count, lookup_capacity;
    sl_replay_ref *refs; /* per file: its sites, then its requests, in call order */
    size_t ref_count, ref_capacity;
    size_t *file_refs; /* refs of file j end at file_refs[j] */
} sl_replay;
/* Recordings by family and name (open addressing over list heads). */
typedef struct sl_store { sl_replay **slots; size_t count, capacity, records; } sl_store;
/* Retained reverse graph (R-386): the shape of the stock reverse base one capture discovered (sl_reverse_roots),
 * kept in the memo for captures with the same catalog, native and renderer key. Strings live in one text block
 * (offset 0 is the empty string, UINT32_MAX no string); units, their paths, dependency targets, site and implicit
 * edges and reader gaps index into the arrays below. */
typedef struct sl_rg_unit {
    uint32_t family, name;              /* text offsets */
    uint32_t files, file_count;         /* paths[files ...]: each file's engine path */
    uint32_t witnesses, witness_count;  /* paths[witnesses ...]: each witness resource's engine path */
    uint32_t deps, dep_count;           /* targets[deps ...]: unit indices */
    uint32_t sites, site_count;         /* edges[sites ...] */
    uint32_t implicits, implicit_count; /* edges[implicits ...] */
    uint32_t gap;                       /* 1 + the gaps index of the reader gap accounted while it was read, else 0 */
} sl_rg_unit;
typedef struct sl_rg_edge { uint32_t slot, target; } sl_rg_edge;
typedef struct sl_rg_gap { uint32_t family, name, reason; } sl_rg_gap;
typedef struct sl_reverse_graph {
    unsigned char key[32];
    uint32_t unit_count, root_count, gap_count, target_count, edge_count, path_count, text_length;
    sl_rg_unit *units; uint32_t *targets, *paths; sl_rg_edge *edges; sl_rg_gap *gaps; char *text;
} sl_reverse_graph;
struct sh_package_source_memo {
    volatile long busy; /* borrowed by one capture */
    int keyed;
    unsigned char key[32]; /* sl_memo_key of the captures that recorded */
    unsigned long long captures;
    sl_store store;
    sl_reverse_graph *reverse; /* the retained reverse graph, or NULL */
};
/* Shared stock (R-344 stage A; sl_fold_scope). A package scope's map: the baseline units its copies became or were
 * replaced by, sorted by family and name once sealed. A scope's pending units: those created and not yet discovered,
 * in creation order, while discovery runs scope by scope. */
typedef struct sl_share_map { struct sl_unit **units; size_t count, capacity, sorted; } sl_share_map;
typedef struct sl_pending {
    struct sl_unit **units; size_t head, count, capacity;
    /* The scope's units admission discovered (its seeds): never folded, but they bind what its folds replace. */
    struct sl_unit **admitted; size_t admitted_count, admitted_capacity;
} sl_pending;
/* What one captured original declaration states for sharing, read once for every scope that reaches it: registration
 * key sites (keys: 1 some, 0 none, -1 unknown) and AI registration lists (conductor). */
typedef struct sl_stock_fact { const sh_compiled_resource *file; const char *family, *name; int keys, conductor; } sl_stock_fact;
/* Mode 2 (decide without sharing): each decided unit's representative, by unit and by family and name. */
typedef struct sl_rep { const struct sl_unit *unit; struct sl_unit *rep; } sl_rep;
/* Unchanged-scope reuse (R-348; sl_reuse_*). A record: per scope its key and bindings, each naming a stock entry
 * (the family and name a scope bound as shared stock, the bound unit's fold structure digest, its certificate
 * semantics then, and the paths the scope's copy consulted). Strings are the record's own. */
typedef struct sl_reuse_stock {
    const char *family, *name;
    unsigned char digest[32], semantics[32];
    size_t path_first, path_count;
} sl_reuse_stock;
typedef struct sl_reuse_binding { size_t stock; unsigned pass; } sl_reuse_binding;
typedef struct sl_reuse_scope { unsigned char key[32]; size_t first, count; } sl_reuse_scope;
/* In a capture: a consulted set (paths in the capture's strings) by recording; a binding a fold or a restore made
 * (set, or the earlier record's stock entry it restored); a recorded binding offered to the scope being discovered. */
/* supplied: some input was its recording scope's own file (the paths kept are the others). */
/* baseline: whether the baseline supplies one of its paths, asked once for every scope judged against it (0 not
 * asked yet, 1 none, 2 some). */
typedef struct sl_reuse_set {
    const struct sl_replay *replay; uint64_t serial; size_t first, count; int supplied; unsigned char baseline;
} sl_reuse_set;
typedef struct sl_reuse_note {
    struct sl_unit *stock; const sl_reuse_stock *from; uint32_t scope, set; unsigned pass;
} sl_reuse_note;
enum { SL_REUSE_REFUSED = 0, SL_REUSE_VALID = 1, SL_REUSE_BOUND = 2 };
typedef struct sl_reuse_candidate {
    const sl_reuse_stock *record;
    struct sl_unit *pool, *copy;
    unsigned char semantics[32]; /* the certificate this scope's copy would carry (sl_fold_certificate) */
    int state, certify, reason;
} sl_reuse_candidate;
typedef struct sl_reuse_digest { const struct sl_unit *unit; unsigned char digest[32]; } sl_reuse_digest;
struct sh_package_source_link {
    const sh_package_compilation **scopes;
    sh_package_source_reader **readers;
    size_t scope_count;
    /* Per scope: 1 + the place of its canonicalizer among the capture's distinct ones (sl_lookup_marked), 0 none. */
    unsigned char *canonical_marks;
    size_t canonical_count;
    /* sh_package_source_link_recovered: sorted unique image names recovered in place. */
    char **recovered;
    size_t recovered_count;
    const sh_resource_catalog *catalog;
    sh_package_baseline_reader original;
    void *original_context;
    sh_package_source_link_native native;
    sh_package_source_shader shader;
    sl_unit **units, **unit_slots;
    size_t count, capacity, unit_slot_count;
    /* Discovery lists until the freeze; then every unit's site edges and runs in one block each, until close. */
    sl_arena dependency_arena, site_arena;
    sl_arena discovery_arena; /* every unit's sl_discovery until the freeze */
    sh_package_link_edge *site_edge_block;
    sl_site_run *site_run_block;
    sl_original **originals, **original_slots;
    size_t original_count, original_capacity, original_slot_count;
    sh_package_link_inputs inputs;
    /* The unit table once lent (inputs.units is NULL then), with room for table_capacity rows; freed at close. */
    sh_package_link_unit *table;
    size_t table_capacity;
#if defined(SH_PACKAGE_LIBRARY_TESTING) || defined(SH_PACKAGE_SOURCE_LINK_TESTING)
    sh_package_link_unit *frozen_rows; /* tests: every row as frozen, to find rows changed without a kept copy */
#endif
    unsigned char *shared;
    /* Product outputs one scope's changed inputs generate (sl_scope_product). */
    struct sl_scope_product { size_t scope; sh_compiled_resource *resource; } *scope_products;
    size_t scope_product_count, scope_product_capacity;
    int frozen, failed, observe;
    int product_optional; /* a producer's refusal is an absent original (sl_seed_modules) */
    char *error;
    size_t error_capacity;
    sh_package_reverse_gap *reverse_gaps;
    size_t reverse_gap_count, reverse_gap_capacity;
    sh_package_shader_unpublished *unpublished; /* sl_shader_accounted's accounting */
    size_t unpublished_count, unpublished_capacity;
    int unpublished_lost;
    int reader_gap; /* The last discovery stopped in a registered reader, not I/O. */
    int preparing; /* Last unit has an explicitly retained native program request. */
    char preparation_reason[1024]; /* First deferred request in this frontier. */
    int malformed; /* That reader could not parse the text in the native grammar. */
    int environment; /* A failure came from I/O, allocation or configuration. */
    /* sl_defer_failure: the first failure a scope alone answers for, kept
     * while the other scopes continue, and the scopes withdrawn for one. */
    struct { const sl_unit *unit; int malformed, reader_gap; char error[1024]; } deferred;
    unsigned char *withdrawn;
    sl_unit *discovering; /* Unit whose readers are requesting new units. */
    const sl_unit *failed_unit; /* Unit whose strict discovery failed, if known. */
    size_t failed_scope; /* Input scope whose supplied files failed seeding. */
    char seed_note[320]; /* Current seeding site, copied into units it creates. */
    char **player_hands; /* Names of animation webs a player's handsAnimWeb names. */
    size_t player_hand_count;
    /* AI state machines (sl_fsm_edges): the catalog's SnapMap declaration
     * names, and masks by the exact source file each scope resolves. */
    char **fsm_names;
    size_t fsm_name_count;
    int fsm_names_ready;
    struct sl_fsm_mask *fsm_masks;
    size_t fsm_mask_count, fsm_mask_capacity;
    /* Installed module members by module path (sl_module_rows). */
    struct sl_module_row *module_rows;
    size_t module_row_count, module_row_capacity;
    int module_rows_ready;
    unsigned long long phase_ms[9]; /* Wall-clock capture phases, see sl_phase_names. */
    /* Process memory after each phase call, in call order (diagnostics only). */
    struct { int phase; unsigned long long mb[4]; } memory_calls[24];
    size_t memory_call_count;
    /* Discovery replay: the discovery being recorded, and the recordings:
     * this capture's own, or those of a borrowed memo. */
    sl_replay *recording;
    sl_store own, *store;
    sh_package_source_memo *memo;
    struct sl_rebuilt *rebuilt; /* a stock reverse base rebuilt from a retained graph (sl_reverse_rebuild); close frees its blocks */
    unsigned long long capture;
    /* The readers' native digest, once per capture (sl_native_digest). */
    unsigned char native_digest[32];
    int native_digested;
    sl_lookup_cache lookup;
    sh_package_source_lookup_counters lookup_counts;
    /* Interned unit families and names and site and implicit slots (sl_intern), freed when the capture closes. */
    char **strings;
    size_t string_count, string_capacity;
    /* Diagnostics only (sh_package_source_link_counters). */
    struct {
        size_t units, binds, original_binds, originals, replayed, recorded, declined, reverse_units;
        int reverse_retained; /* the stock reverse base was rebuilt from the memo's retained graph (R-386) */
        size_t retained; /* replays of an earlier capture's recording */
        size_t campaign_boundaries; /* stock-route campaign-only references kept as identities (sl_files) */
        int memo; /* 0 none, 1 kept, 2 renewed for a different key, 3 busy */
        unsigned long long original_bytes;
        unsigned long long select_us, read_us, construct_us; /* discovery sub-phases */
        size_t replay_tries; /* recorded reads validated against a scope */
        unsigned long long applies_us, apply_us; /* within read_us: replay validation, application */
        unsigned long long fresh_us; /* within read_us: reads of units not replayed */
        unsigned long long original_us, identify_us; /* capturing and hashing originals */
        unsigned long long companion_us; /* within select_us: material companion requests */
        unsigned long long applies_without_capture_us;
    } census;
    /* Shared stock (R-344 stage A): see sh_package_source_link_share and sl_fold_scope. */
    struct {
        int mode;          /* 0 none, 1 share, 2 decide only (oracle) */
        int scope_major;   /* discovery runs scope by scope: sl_find queues each new unit on its scope's pending list */
        size_t baseline;   /* the stock scope (root plan baseline_scope) */
        unsigned char *fold; /* per scope: its proven stock copies may be shared */
        unsigned char *fsm_inputs; /* per scope: it supplies aifsmmanager or aibehavior declarations */
        sl_share_map *maps;
        sl_pending *pending;
        struct sl_unit **batch; /* the units the scope being folded discovered in its last pass */
        size_t batch_count, batch_capacity;
        sl_stock_fact *facts; /* open addressing by file */
        size_t fact_count, fact_capacity;
        sl_rep *reps, *rep_names; /* mode 2, open addressing by unit and by family and name */
        size_t rep_count, rep_capacity, rep_name_count, rep_name_capacity;
        struct { size_t scope; char *family, *name; } privatize; /* tests: a forced later trigger */
        /* Diagnostics (sh_package_source_link_counters). */
        size_t folds, decided, folded, rehomed, promoted, privatized, privatizations, survivors, stock_units;
        size_t certified, certificates_reused, key_scans, conductors;
        size_t blocked_local, blocked_inputs, blocked_family, blocked_keys, blocked_closure, blocked_equal;
        size_t peak_live, live; /* live units: the discovery transient */
        unsigned long long fold_us, certify_us, keys_us;
        /* Streamed binary originals: let go after their first recorded discovery, and read again since. */
        size_t released;
        unsigned long long released_bytes;
        size_t rereads;
        unsigned long long reread_bytes, reread_us;
    } share;
    /* Unchanged-scope reuse (R-348; see sl_reuse_begin). */
    struct {
        sh_package_source_reuse *out;          /* receives this capture's record when it completes */
        const sh_package_source_reuse *in;     /* an earlier capture's record */
        unsigned char (*keys)[32];             /* per package scope with folds: sl_reuse_scope_key */
        unsigned char *keyed;
        size_t *matched;                       /* per scope: its record in `in`, SIZE_MAX none */
        unsigned pass;                         /* the discovery pass sl_discover_scopes runs (1-3) */
        sl_reuse_set *sets; size_t set_count, set_capacity;
        uint32_t *set_slots; size_t set_slot_capacity;
        const char **paths; size_t path_count, path_capacity;
        unsigned char *path_marks; size_t path_mark_capacity; /* per path (sl_lookup_marked) */
        sl_reuse_note *marks; size_t mark_count, mark_capacity;
        sl_reuse_candidate *candidates; size_t candidate_count, candidate_capacity;
        uint32_t *candidate_slots; size_t candidate_slot_capacity;
        size_t table_scope;                    /* the scope whose candidates are current, SIZE_MAX none */
        sl_reuse_digest *digests; size_t digest_count, digest_capacity;
        sh_package_source_reuse_counters counts;
    } reuse;
};
static int g_sl_replay = 1;
void sh_package_source_link_replay(int enabled) { g_sl_replay = enabled != 0; }
static int g_sl_lookup = 0;
#define SL_LOOKUP_LIMIT (32u * 1024u * 1024u)
static size_t g_sl_lookup_limit = SL_LOOKUP_LIMIT;
void sh_package_source_link_lookup_memo(int enabled) { g_sl_lookup = enabled != 0; }
#ifdef SH_PACKAGE_LIBRARY_TESTING
static long g_sl_lookup_failures;
static int g_sl_lookup_collisions;
void sh_package_source_link_test_lookup(size_t limit, long failures, int collisions)
{
    g_sl_lookup_limit = limit < SL_LOOKUP_LIMIT ? limit : SL_LOOKUP_LIMIT;
    g_sl_lookup_failures = failures; g_sl_lookup_collisions = collisions != 0;
}
#endif
static int g_sl_fsm = 1;
void sh_package_source_link_fsm(int enabled) { g_sl_fsm = enabled != 0; }
static int g_sl_share = 1, g_sl_stream = 1;
void sh_package_source_link_share(int mode) { g_sl_share = mode >= 0 && mode <= 2 ? mode : 1; }
void sh_package_source_link_stream(int enabled) { g_sl_stream = enabled != 0; }
#if defined(SH_PACKAGE_LIBRARY_TESTING) || defined(SH_PACKAGE_SOURCE_LINK_TESTING) || \
    defined(SH_RESOURCE_GRAPH_TESTING) || defined(SH_PACKAGE_RUNTIME_TESTING)
/* Test builds only: a whole suite runs in another shared-stock mode (SMP_SHARE_MODE 0, 1 or 2), without the
 * conservative non-sharing (SMP_SHARE_CONSERVATIVE=0) or without streamed originals (SMP_STREAM=0), unchanged, as the
 * differential oracle across its fixtures. Read once, before the first capture. */
static void sl_test_environment(void);
#define SL_TEST_ENVIRONMENT() sl_test_environment()
#else
#define SL_TEST_ENVIRONMENT() ((void)0)
#endif
#if defined(SH_PACKAGE_LIBRARY_TESTING) || defined(SH_PACKAGE_SOURCE_LINK_TESTING)
/* Tests: refuse the nth shared-stock allocation (0: none); force a later privatization; shared rows edited. */
static long g_sl_share_refusal;
static size_t g_sl_privatize_scope = SIZE_MAX;
static char g_sl_privatize_family[64], g_sl_privatize_name[256];
static size_t g_sl_shared_rows_checked, g_sl_shared_rows_changed;
void sh_package_source_link_test_share_refusal(long nth) { g_sl_share_refusal = nth; }
long sh_package_source_link_test_share_refusal_left(void) { return g_sl_share_refusal; }
void sh_package_source_link_test_privatize(size_t scope, const char *family, const char *name)
{
    g_sl_privatize_scope = family && name ? scope : SIZE_MAX;
    snprintf(g_sl_privatize_family, sizeof(g_sl_privatize_family), "%s", family ? family : "");
    snprintf(g_sl_privatize_name, sizeof(g_sl_privatize_name), "%s", name ? name : "");
}
void sh_package_source_link_test_shared_rows(size_t *checked, size_t *changed)
{ *checked = g_sl_shared_rows_checked; *changed = g_sl_shared_rows_changed; }
#define SL_SHARE_REFUSED() (g_sl_share_refusal > 0 && !--g_sl_share_refusal)
#else
#define SL_SHARE_REFUSED() 0
#endif
#if defined(SH_PACKAGE_LIBRARY_TESTING) || defined(SH_PACKAGE_SOURCE_LINK_TESTING) ||     defined(SH_RESOURCE_GRAPH_TESTING) || defined(SH_PACKAGE_RUNTIME_TESTING)
/* Tests (R-348): refuse the nth reuse allocation or binding; drop validations of offered bindings (a bit each). */
static long g_sl_reuse_refusal;
static int g_sl_reuse_mutation;
void sh_package_source_link_test_reuse_refusal(long nth) { g_sl_reuse_refusal = nth; }
long sh_package_source_link_test_reuse_refusal_left(void) { return g_sl_reuse_refusal; }
void sh_package_source_link_test_reuse_mutation(int clause) { g_sl_reuse_mutation = clause; }
#define SL_REUSE_REFUSED() (g_sl_reuse_refusal > 0 && !--g_sl_reuse_refusal)
#define SL_REUSE_MUTATED(clause) ((g_sl_reuse_mutation >> (clause)) & 1)
/* SMP_REUSE_DEBUG=1: every offered binding's judgment and every binding on stderr. */
static int sl_reuse_debug(void)
{
    static volatile long read; static int on;
    if (!_InterlockedCompareExchange(&read, 1, 0)) {
        char *value = NULL; size_t length = 0;
        on = !_dupenv_s(&value, &length, "SMP_REUSE_DEBUG") && value && *value == '1';
        free(value);
    }
    return on;
}
#define SL_REUSE_DEBUG() sl_reuse_debug()
#else
#define SL_REUSE_REFUSED() 0
#define SL_REUSE_MUTATED(clause) 0
#define SL_REUSE_DEBUG() 0
#endif
static const char *const sl_phase_names[9] = {
    "readers", "seeds", "reverse roots", "discovery", "shaders", "audio", "certification", "owners", "inputs"
};
static unsigned long long sl_now_ms(void)
{
    struct timespec now;
    return timespec_get(&now, TIME_UTC) ? (unsigned long long)now.tv_sec * 1000u + (unsigned long long)now.tv_nsec / 1000000u : 0;
}
static unsigned long long sl_now_us(void)
{
    struct timespec now;
    return timespec_get(&now, TIME_UTC) ? (unsigned long long)now.tv_sec * 1000000u + (unsigned long long)now.tv_nsec / 1000u : 0;
}
static void sl_phase(sh_package_source_link *s, int phase, unsigned long long *mark)
{
    unsigned long long now = sl_now_ms();
    s->phase_ms[phase] += now >= *mark ? now - *mark : 0; *mark = now;
    if (s->memory_call_count < sizeof(s->memory_calls) / sizeof(s->memory_calls[0]) &&
        sh_package_memory_read(s->memory_calls[s->memory_call_count].mb))
        s->memory_calls[s->memory_call_count++].phase = phase;
}

static uint64_t sl_hash_text(uint64_t hash, const char *text)
{
    for (; *text; text++) { hash ^= (unsigned char)*text; hash *= UINT64_C(1099511628211); }
    return (hash ^ 0xff) * UINT64_C(1099511628211);
}
static uint64_t sl_unit_hash(size_t scope, const char *family, const char *name)
{ return sl_hash_text(sl_hash_text(UINT64_C(14695981039346656037) ^ scope, family), name); }
static uint64_t sl_lookup_hash(size_t scope, const char *key, const sh_compiled_resource *file)
{
#ifdef SH_PACKAGE_LIBRARY_TESTING
    if (g_sl_lookup_collisions) return 0;
#endif
    uint64_t seed = UINT64_C(14695981039346656037) ^ scope;
    return key ? sl_hash_text(seed, key) : (seed ^ (uint64_t)(uintptr_t)file) * UINT64_C(1099511628211);
}
static size_t sl_lookup_slot(const sl_lookup_cache *cache, uint64_t hash, size_t scope,
    const char *key, const sh_compiled_resource *file)
{
    size_t at = (size_t)hash & (cache->capacity - 1);
    int kind = key ? 1 : 2;
    while (cache->slots[at].kind) {
        const sl_lookup_entry *entry = cache->slots + at;
        if (entry->hash == hash && entry->scope == scope && entry->kind == kind &&
            (key ? !strcmp(entry->key, key) : entry->file == file)) break;
        at = (at + 1) & (cache->capacity - 1);
    }
    return at;
}
static void *sl_lookup_allocate(size_t bytes, int clear)
{
#ifdef SH_PACKAGE_LIBRARY_TESTING
    if (g_sl_lookup_failures > 0) { g_sl_lookup_failures--; return NULL; }
#endif
    return clear ? calloc(1, bytes) : malloc(bytes);
}
static int sl_lookup_grow(sl_lookup_cache *cache)
{
    size_t next = cache->capacity ? cache->capacity * 2 : 64;
    if (next <= cache->capacity || next > SIZE_MAX / sizeof(*cache->slots)) return 0;
    size_t bytes = next * sizeof(*cache->slots);
    /* The budget also covers the temporary old+new slot arrays during growth. */
    if (cache->bytes > cache->limit || bytes > cache->limit - cache->bytes) return 0;
    sl_lookup_entry *slots = sl_lookup_allocate(bytes, 1);
    if (!slots) return 0;
    sl_lookup_cache grown = *cache; grown.slots = slots; grown.capacity = next;
    for (size_t i = 0; i < cache->capacity; i++) if (cache->slots[i].kind) {
        const sl_lookup_entry *entry = cache->slots + i;
        slots[sl_lookup_slot(&grown, entry->hash, entry->scope, entry->key, entry->file)] = *entry;
    }
    if (cache->peak < cache->bytes + bytes) cache->peak = cache->bytes + bytes;
    cache->bytes += bytes - cache->capacity * sizeof(*cache->slots);
    free(cache->slots); cache->slots = slots; cache->capacity = next; return 1;
}
static sl_lookup_entry *sl_lookup_get(sh_package_source_link *s, size_t scope,
    const char *key, const sh_compiled_resource *file, int add)
{
    sl_lookup_cache *cache = &s->lookup;
    if (!cache->limit) return NULL;
    uint64_t hash = sl_lookup_hash(scope, key, file);
    if (cache->capacity) {
        size_t at = sl_lookup_slot(cache, hash, scope, key, file);
        if (cache->slots[at].kind) return cache->slots + at;
    }
    if (!add || (cache->count >= cache->capacity / 2 && !sl_lookup_grow(cache))) return NULL;
    size_t length = key ? strlen(key) + 1 : 0;
    if (cache->bytes > cache->limit || length > cache->limit - cache->bytes) return NULL;
    char *copy = length ? sl_lookup_allocate(length, 0) : NULL;
    if (length && !copy) return NULL;
    if (length) memcpy(copy, key, length);
    size_t at = sl_lookup_slot(cache, hash, scope, key, file);
    sl_lookup_entry *entry = cache->slots + at;
    entry->hash = hash; entry->scope = scope; entry->key = copy; entry->file = file;
    entry->kind = key ? 1 : 2; cache->count++; cache->bytes += length;
    if (cache->peak < cache->bytes) cache->peak = cache->bytes;
    return entry;
}
/* The supplied search for a canonical key of the scope (borrowed). */
static const sh_compiled_resource *sl_lookup_key(sh_package_source_link *s, size_t scope, const char *key)
{
    const sh_package_compilation *compiled = s->scopes[scope];
    if (!compiled->resource_count) { s->lookup_counts.searches++; return NULL; }
    sl_lookup_entry *entry = sl_lookup_get(s, scope, key, NULL, 0);
    if (entry) { s->lookup_counts.lookup_hits++; return entry->file; }
    size_t low = 0, high = compiled->resource_count;
    const sh_compiled_resource *found = NULL;
    s->lookup_counts.searches++;
    while (low < high) {
        size_t mid = low + (high - low) / 2u;
        int compare = strcmp(key, compiled->resources[mid].engine_path);
        if (!compare) { found = compiled->resources + mid; break; }
        if (compare < 0) high = mid; else low = mid + 1u;
    }
    entry = sl_lookup_get(s, scope, key, NULL, 1);
    if (entry) entry->file = found;
    return found;
}
/* Same canonicalizer call and binary search as sh_package_compilation_find.
 * Callers that canonicalized first still do so: the resulting original path
 * must not be substituted with this second supplied-search key. */
static const sh_compiled_resource *sl_lookup_supplied(sh_package_source_link *s, size_t scope, const char *path)
{
    const sh_package_compilation *compiled = s->scopes[scope];
    char *key = compiled && compiled->canonical_path ? compiled->canonical_path(path) : sh_package_engine_path(path);
    s->lookup_counts.lookups++;
    if (!compiled || !key) { free(key); return NULL; }
    const sh_compiled_resource *found = sl_lookup_key(s, scope, key);
    free(key); return found;
}
/* sl_lookup_supplied for a path that keeps a mark (a recording's input, a consulted path) through the capture: it is
 * canonicalized until it proves to be its own key under the scope's canonicalizer, which the mark then names, and
 * searched by itself from then on (R-348: replay validation, shared-input checks and reuse judging look the same
 * paths up in every scope). */
static const sh_compiled_resource *sl_lookup_marked(sh_package_source_link *s, size_t scope, const char *path,
    unsigned char *mark)
{
    const sh_package_compilation *compiled = s->scopes[scope];
    unsigned char want = s->canonical_marks ? s->canonical_marks[scope] : 0;
    if (compiled && want && *mark == want) {
        s->lookup_counts.lookups++; s->lookup_counts.marked++;
        return sl_lookup_key(s, scope, path);
    }
    char *key = compiled && compiled->canonical_path ? compiled->canonical_path(path) : sh_package_engine_path(path);
    s->lookup_counts.lookups++;
    if (!compiled || !key) { free(key); return NULL; }
    if (want && !strcmp(key, path)) *mark = want;
    const sh_compiled_resource *found = sl_lookup_key(s, scope, key);
    free(key); return found;
}
/* A recording's input looked up through its mark. Marks hold for the capture that set them: a recording a later
 * capture replays (a memo's) starts unmarked there. They are the capture's cache, not part of the recording. */
static const sh_compiled_resource *sl_lookup_input(sh_package_source_link *s, size_t scope, const struct sl_replay *r,
    const sl_replay_input *input)
{
    struct sl_replay *own = (struct sl_replay *)r;
    if (own->marked != s->capture) {
        for (size_t i = 0; i < own->file_count; i++) own->files[i].mark = 0;
        for (size_t i = 0; i < own->lookup_count; i++) own->lookups[i].mark = 0;
        own->marked = s->capture;
    }
    return sl_lookup_marked(s, scope, input->path, &((sl_replay_input *)input)->mark);
}
static int sl_lookup_identity(sh_package_source_link *s, size_t scope, const sh_compiled_resource *file,
    sh_package_file_identity *out, char *error, size_t capacity)
{
    s->lookup_counts.identities++;
    sl_lookup_entry *entry = sl_lookup_get(s, scope, NULL, file, 0);
    if (entry) { s->lookup_counts.identity_hits++; *out = entry->identity; return 1; }
    s->lookup_counts.identity_computations++;
    if (file->body) s->lookup_counts.body_hashes++;
    if (!sh_package_compilation_identity(s->scopes[scope], file, out, error, capacity)) return 0;
    entry = sl_lookup_get(s, scope, NULL, file, 1);
    if (entry) entry->identity = *out;
    return 1;
}
static int sl_fail(sh_package_source_link *s, const char *reason, const char *name)
{
    s->failed = 1;
    if (s->error && s->error_capacity && !*s->error)
        snprintf(s->error, s->error_capacity, "%s%s%s", reason, name ? ": " : "", name ? name : "");
    return 0;
}
/* I/O, allocation, internal state or configuration: never evidence against
 * the input whose closure was being read. */
static int sl_env_fail(sh_package_source_link *s, const char *reason, const char *name)
{
    s->environment = 1;
    return sl_fail(s, reason, name);
}
/* Continue discovering independent units, but never certify or publish this
 * incomplete frontier. A fresh pass must consume the prepared native output. */
static int sl_defer_preparation(sh_package_source_link *s)
{
    s->preparing = 1;
    if (!*s->preparation_reason) snprintf(s->preparation_reason, sizeof(s->preparation_reason), "%s",
        s->error && *s->error ? s->error : "shader environment requires native preparation");
    if (s->error && s->error_capacity) *s->error = 0;
    return 0;
}
/* Missing product coverage for a source that was read successfully. Ordinary
 * linking still refuses it. Reverse observation of stock roots accounts it as
 * a gap in that root instead; only I/O and allocation failures stay fatal. */
static int sl_gap_fail(sh_package_source_link *s, const char *reason, const char *name)
{
    if (!s->observe) return sl_fail(s, reason, name);
    s->reader_gap = 1;
    if (s->error && s->error_capacity && !*s->error)
        snprintf(s->error, s->error_capacity, "%s%s%s", reason, name ? ": " : "", name ? name : "");
    return 0;
}
#if defined(SH_PACKAGE_LIBRARY_TESTING) || defined(SH_PACKAGE_SOURCE_LINK_TESTING)
/* Tests: refuse the nth spelling or prepared stand-in a capture would keep (0: none), and capture originals as
 * copies even when a borrowing reader is given. */
static long g_sl_intern_refusal, g_sl_prepared_refusal;
static int g_sl_borrow = 1;
void sh_package_source_link_test_refuse(long intern, long prepared)
{ g_sl_intern_refusal = intern; g_sl_prepared_refusal = prepared; }
void sh_package_source_link_test_refusals_left(long *intern, long *prepared)
{ *intern = g_sl_intern_refusal; *prepared = g_sl_prepared_refusal; }
void sh_package_source_link_test_borrow(int enabled) { g_sl_borrow = enabled != 0; }
/* Tests: 0 lets readers copy an original only to learn its status, as before R-342; status queries answered. */
static int g_sl_status = 1;
static size_t g_sl_status_queries;
void sh_package_source_link_test_status(int enabled) { g_sl_status = enabled != 0; }
size_t sh_package_source_link_test_status_queries(void) { return g_sl_status_queries; }
static int sl_test_refused(long *countdown) { return *countdown > 0 && !--*countdown; }
#define SL_REFUSED(countdown) sl_test_refused(&(countdown))
#define SL_BORROW g_sl_borrow
#define SL_STATUS g_sl_status
#define SL_STATUS_QUERY() (g_sl_status_queries++)
/* Tests: every arena block holds this many bytes (0: the default growth); refuse the nth block or frozen site block. */
static size_t g_sl_block_bytes;
static long g_sl_block_refusal;
/* Tests: refuse the nth hand-over of the frozen unit table (0: none), as an exhausted heap would. */
static long g_sl_take_refusal;
/* Tests: refuse the nth optional unit record (0: none), as an exhausted heap would (R-343). */
static long g_sl_extra_refusal;
void sh_package_source_link_test_extra_refusal(long nth) { g_sl_extra_refusal = nth; }
long sh_package_source_link_test_extra_refusal_left(void) { return g_sl_extra_refusal; }
void sh_package_source_link_test_take_refusal(long nth) { g_sl_take_refusal = nth; }
long sh_package_source_link_test_take_refusal_left(void) { return g_sl_take_refusal; }
void sh_package_source_link_test_blocks(size_t bytes, long refuse) { g_sl_block_bytes = bytes; g_sl_block_refusal = refuse; }
long sh_package_source_link_test_blocks_left(void) { return g_sl_block_refusal; }
#define SL_BLOCK_BYTES(grown) (g_sl_block_bytes ? g_sl_block_bytes : (grown))
#else
#define SL_REFUSED(countdown) 0
#define SL_BORROW 1
#define SL_STATUS 1
#define SL_STATUS_QUERY() ((void)0)
#define SL_BLOCK_BYTES(grown) (grown)
#endif
#if defined(SH_PACKAGE_LIBRARY_TESTING) || defined(SH_PACKAGE_SOURCE_LINK_TESTING)
/* Tests (R-343): refuse the nth kept copy of a lent table's row (0: none), as an exhausted heap would; rows compared
 * with their frozen copies and those found changed although the capture kept no copy, since the process started. */
static long g_sl_preserve_refusal;
static size_t g_sl_rows_checked, g_sl_rows_changed, g_sl_rows_kept;
static int g_sl_rows_strict = 1;
void sh_package_source_link_test_strict_rows(int strict) { g_sl_rows_strict = strict != 0; }
void sh_package_source_link_test_preserve_refusal(long nth) { g_sl_preserve_refusal = nth; }
long sh_package_source_link_test_preserve_refusal_left(void) { return g_sl_preserve_refusal; }
void sh_package_source_link_test_rows(size_t *checked, size_t *changed, size_t *kept)
{ *checked = g_sl_rows_checked; *changed = g_sl_rows_changed; *kept = g_sl_rows_kept; }
static int sl_row_same(const sh_package_link_unit *a, const sh_package_link_unit *b)
{
    return a->family == b->family && a->name == b->name && a->compilation == b->compilation && a->files == b->files &&
        a->file_count == b->file_count && !memcmp(a->semantics, b->semantics, 32) &&
        a->original_binding == b->original_binding && a->witnesses == b->witnesses &&
        a->witness_count == b->witness_count && a->file_identities == b->file_identities && a->implicit == b->implicit &&
        a->implicit_count == b->implicit_count && a->conditional == b->conditional &&
        a->conditional_count == b->conditional_count && a->implicit_owners == b->implicit_owners &&
        a->implicit_gameplay_owners == b->implicit_gameplay_owners && a->stable_name == b->stable_name &&
        a->fixed_name == b->fixed_name;
}
#endif
static int sl_grow_from(void ***items, size_t *capacity, size_t count, size_t first)
{
    if (count < *capacity) return 1;
    size_t next = *capacity ? *capacity * 2 : first;
    void **grown;
    if (next <= *capacity || next > SIZE_MAX / sizeof(*grown) || !(grown = realloc(*items, next * sizeof(*grown)))) return 0;
    *items = grown; *capacity = next; return 1;
}
static int sl_grow(void ***items, size_t *capacity, size_t count) { return sl_grow_from(items, capacity, count, 32); }
/* A unit's own files and dependencies. Most units have one file and few dependencies, and a capture holds up to
 * about a million units, so these start small. */
static int sl_grow_unit(void ***items, size_t *capacity, size_t count) { return sl_grow_from(items, capacity, count, 4); }
/* ---- Discovery lists -------------------------------------------------------
 * A unit's dependencies and reference sites grow while the capture discovers
 * it and are read only until the inputs freeze: the frozen capture keeps the
 * sites as link edges with per-file runs (sh_package_source_link_sites), and no
 * reader follows a unit's dependencies after it. A complete library captures
 * about a million units with nine million dependencies and seven million
 * sites, so these lists live in two capture-owned arenas of large blocks, not
 * in a million heap lists that double. A list grows in place while it ends its
 * arena's newest block, as it does while its unit is being discovered;
 * otherwise it moves to the arena's end with room to double, and its old place
 * stays unused until the arena is released. Each unit's other discovery state
 * (sl_discovery) is one record in a third arena. The freeze releases all three
 * whole (sl_discovery_release), returning their blocks to the system rather
 * than to the heap's small-block buckets. */
#define SL_ARENA_FIRST ((size_t)64 << 10)
#define SL_ARENA_LARGEST ((size_t)4 << 20)
/* Room for one item after count in the list *items of *capacity items of size bytes, in arena a. 0 when it
 * cannot grow; the list is then unchanged. */
/* A unit's optional record, allocated by its first writer: NULL when it cannot be (the writer then fails, or, for a
 * diagnostic note, keeps none). */
static sl_extra *sl_extra_of(sl_unit *unit)
{
    if (!unit->extra && !SL_REFUSED(g_sl_extra_refusal)) unit->extra = calloc(1, sizeof(*unit->extra));
    return unit->extra;
}
/* What a unit without the optional record reads: no programs, files, configuration, identities, preparation or note. */
static const sl_extra sl_no_extra;
#define SL_EXTRA(u) ((u)->extra ? (const sl_extra *)(u)->extra : &sl_no_extra)
/* A region of capacity items no list holds any more: kept for reuse by a list of at most 2^floor(log2 capacity)
 * items (its first bytes link the free regions of that class). */
static void sl_arena_release(sl_arena *a, void *items, uint32_t capacity, size_t size)
{
    unsigned c = 0;
    if (!items || !capacity || (size_t)capacity * size < sizeof(void *)) return;
    while (c < 31 && ((uint32_t)2 << c) <= capacity) c++;
    *(void **)items = a->free[c]; a->free[c] = items;
}
/* A region of at least next (<= UINT32_MAX) items of size bytes: a released one of the smallest class that holds
 * them (shared stock frees whole units, and every list that moves leaves its old place there), else room at the end
 * of the newest block, else a new block. *capacity receives the items it holds; NULL when none can be had. */
static void *sl_arena_take(sl_arena *a, size_t next, size_t size, uint32_t *capacity)
{
    unsigned c = 0;
    while (c < 32 && ((uint64_t)1 << c) < next) c++;
    if (c < 32 && a->free[c]) {
        void *reused = a->free[c];
        a->free[c] = *(void **)reused;
        *capacity = (uint32_t)1 << c; return reused;
    }
    if (!next || next > SIZE_MAX / size) return NULL;
    size_t bytes = next * size;
    sl_block *b = a->blocks;
    if (!b || b->size - b->used < bytes) {
        size_t grown = !a->block_size ? SL_ARENA_FIRST : a->block_size < SL_ARENA_LARGEST ? a->block_size * 2 :
            SL_ARENA_LARGEST, block = SL_BLOCK_BYTES(grown);
        if (block < bytes) block = bytes;
        sl_block *added = block <= SIZE_MAX - sizeof(*added) && !SL_REFUSED(g_sl_block_refusal) ?
            malloc(sizeof(*added) + block) : NULL;
        if (!added) return NULL;
        added->next = b; added->size = block; added->used = 0; added->reserved = 0;
        a->blocks = b = added; a->block_size = grown;
    }
    void *taken = (unsigned char *)(b + 1) + b->used;
    b->used += bytes; *capacity = (uint32_t)next; return taken;
}
static int sl_arena_grow(sl_arena *a, void **items, uint32_t *capacity, size_t count, size_t size)
{
    if (count < *capacity) return 1;
    sl_block *b = a->blocks;
    if (*items && b && *capacity < UINT32_MAX &&
        (unsigned char *)*items + (size_t)*capacity * size == (unsigned char *)(b + 1) + b->used &&
        b->size - b->used >= size) {
        b->used += size; ++*capacity; return 1;
    }
    size_t next = count ? count * 2 : 1;
    if (next > UINT32_MAX) next = UINT32_MAX;
    if (next <= count || next > SIZE_MAX / size) return 0;
    uint32_t room = 0;
    void *moved = sl_arena_take(a, next, size, &room);
    if (!moved) return 0;
    if (count) memcpy(moved, *items, count * size);
    sl_arena_release(a, *items, *capacity, size);
    *items = moved; *capacity = room; return 1;
}
static void sl_arena_free(sl_arena *a)
{
    while (a->blocks) { sl_block *next = a->blocks->next; free(a->blocks); a->blocks = next; }
    a->block_size = 0; memset(a->free, 0, sizeof(a->free));
}
#if defined(SH_PACKAGE_LIBRARY_TESTING) || defined(SH_PACKAGE_SOURCE_LINK_TESTING)
int sh_package_source_link_test_arena(char *error, size_t capacity)
{
    static const size_t blocks[] = {0, 1, 16, 48, 4096};
    for (size_t m = 0; m < sizeof(blocks) / sizeof(blocks[0]); m++) {
        sl_arena arena = {0};
        size_t *items[5] = {0}, count[5] = {0}, shadow[5][300], saved = g_sl_block_bytes;
        uint32_t room[5] = {0};
        long refusal = g_sl_block_refusal;
        int ok = 1;
        g_sl_block_bytes = blocks[m]; g_sl_block_refusal = 0;
        /* Bursts on one list, then interleaved growth while other lists follow it. */
        for (size_t step = 0; ok && step < 1200; step++) {
            size_t j = step < 40 ? step / 8 % 5 : (step * 7 + step / 13) % 5;
            if (count[j] == 300) continue;
            if (!sl_arena_grow(&arena, (void **)&items[j], &room[j], count[j], sizeof(size_t))) ok = 0;
            else { items[j][count[j]] = step; shadow[j][count[j]++] = step; }
            for (size_t q = 0; ok && q < 5; q++)
                if (count[q] && memcmp(items[q], shadow[q], count[q] * sizeof(size_t))) ok = 0;
        }
        g_sl_block_bytes = saved; g_sl_block_refusal = refusal;
        sl_arena_free(&arena);
        if (!ok) {
            if (error && capacity) snprintf(error, capacity, "arena lists differ from their shadows with %zu-byte blocks", blocks[m]);
            return 0;
        }
    }
    return 1;
}
#endif
/* The range of a frozen unit's site edges that file bound: its first run, or empty. */
static void sl_site_range(const sl_unit *unit, const sh_compiled_resource *file, size_t *begin, size_t *end)
{
    *begin = *end = 0;
    for (const sl_site_run *run = unit->site_runs; run; *begin = (run++)->end) {
        if (run->file == file) { *end = run->end; return; }
        if (run->end == unit->site_count) { *begin = *end = unit->site_count; return; }
    }
}
/* The capture's one copy of a unit family or name or a site or implicit slot spelling: about a million units
 * repeat far fewer spellings. Owned by the capture until it closes; NULL when it cannot be retained. */
static const char *sl_intern(sh_package_source_link *s, const char *text)
{
    if (SL_REFUSED(g_sl_intern_refusal)) return NULL;
    if (s->string_count >= s->string_capacity / 2) {
        size_t next = s->string_capacity ? s->string_capacity * 2 : 1024;
        char **slots = next > s->string_capacity && next <= SIZE_MAX / sizeof(*slots) ? calloc(next, sizeof(*slots)) : NULL;
        if (!slots) return NULL;
        for (size_t i = 0; i < s->string_capacity; i++) if (s->strings[i]) {
            size_t at = (size_t)sl_hash_text(0, s->strings[i]) & (next - 1);
            while (slots[at]) at = (at + 1) & (next - 1);
            slots[at] = s->strings[i];
        }
        free(s->strings); s->strings = slots; s->string_capacity = next;
    }
    size_t at = (size_t)sl_hash_text(0, text) & (s->string_capacity - 1);
    while (s->strings[at]) {
        if (!strcmp(s->strings[at], text)) return s->strings[at];
        at = (at + 1) & (s->string_capacity - 1);
    }
    char *copy = _strdup(text);
    if (!copy) return NULL;
    s->strings[at] = copy; s->string_count++;
    return copy;
}
static int sl_unit_rehash(sh_package_source_link *s)
{
    size_t next = s->unit_slot_count ? s->unit_slot_count * 2 : 64;
    sl_unit **slots;
    if (next <= s->unit_slot_count || next > SIZE_MAX / sizeof(*slots) || !(slots = calloc(next, sizeof(*slots)))) return 0;
    for (size_t i = 0; i < s->count; i++) {
        sl_unit *unit = s->units[i];
        if (!unit) continue; /* a unit shared stock freed (sl_fold_scope), until sl_units_compact */
        size_t at = (size_t)sl_unit_hash(unit->scope, unit->unit->family, unit->unit->name) & (next - 1);
        while (slots[at]) at = (at + 1) & (next - 1);
        slots[at] = unit;
    }
    free(s->unit_slots); s->unit_slots = slots; s->unit_slot_count = next; return 1;
}
/* The unit of exactly this scope, family and name, without sharing or creating one. */
static sl_unit *sl_find_raw(const sh_package_source_link *s, size_t scope, const char *family, const char *name)
{
    if (!s->unit_slot_count || !family || !name) return NULL;
    size_t at = (size_t)sl_unit_hash(scope, family, name) & (s->unit_slot_count - 1);
    while (s->unit_slots[at]) {
        sl_unit *unit = s->unit_slots[at];
        if (unit->scope == scope && !strcmp(unit->unit->family, family) && !strcmp(unit->unit->name, name)) return unit;
        at = (at + 1) & (s->unit_slot_count - 1);
    }
    return NULL;
}
/* ---- Shared stock: slots, maps and promotion (R-344 stage A) ------------------
 * A unit leaves its slot when it is freed or moves to the baseline scope (sl_fold_scope): linear probing closes the
 * gap by moving back each later unit of the run whose home slot does not lie between the gap and it. */
static void sl_unit_slot_remove(sh_package_source_link *s, const sl_unit *unit)
{
    size_t mask = s->unit_slot_count - 1, at;
    if (!s->unit_slot_count) return;
    at = (size_t)sl_unit_hash(unit->scope, unit->unit->family, unit->unit->name) & mask;
    while (s->unit_slots[at] != unit) {
        if (!s->unit_slots[at]) return;
        at = (at + 1) & mask;
    }
    s->unit_slots[at] = NULL;
    for (size_t next = (at + 1) & mask; s->unit_slots[next]; next = (next + 1) & mask) {
        sl_unit *moved = s->unit_slots[next];
        size_t home = (size_t)sl_unit_hash(moved->scope, moved->unit->family, moved->unit->name) & mask;
        /* It may move back unless its home lies cyclically within (at, next]. */
        int between = at <= next ? home > at && home <= next : home > at || home <= next;
        if (between) continue;
        s->unit_slots[at] = moved; s->unit_slots[next] = NULL; at = next;
    }
}
static void sl_unit_slot_insert(sh_package_source_link *s, sl_unit *unit)
{
    size_t mask = s->unit_slot_count - 1;
    size_t at = (size_t)sl_unit_hash(unit->scope, unit->unit->family, unit->unit->name) & mask;
    while (s->unit_slots[at]) at = (at + 1) & mask;
    s->unit_slots[at] = unit;
}
static int sl_share_order(const void *a, const void *b)
{
    const sl_unit *x = *(sl_unit *const *)a, *y = *(sl_unit *const *)b;
    int order = strcmp(x->unit->family, y->unit->family);
    return order ? order : strcmp(x->unit->name, y->unit->name);
}
static sl_unit *sl_reuse_bound(const sh_package_source_link *s, const char *family, const char *name);
static sl_unit *sl_reuse_bind(sh_package_source_link *s, size_t scope, const char *family, const char *name);
static uint32_t sl_reuse_set_of(sh_package_source_link *s, const struct sl_replay *r);
static int sl_reuse_mark(sh_package_source_link *s, size_t scope, struct sl_unit *stock, uint32_t set,
    const sl_reuse_stock *from);
/* The baseline unit scope binds for family and name instead of its own copy (sl_fold_scope), if any: sealed entries
 * by search, entries of the fold in progress by scan, and those a restored scope bound in the pass under way by its
 * offered bindings (sl_reuse_bind) instead. */
static sl_unit *sl_share_find(const sh_package_source_link *s, size_t scope, const char *family, const char *name)
{
    const sl_share_map *map = s->share.maps && scope < s->scope_count ? s->share.maps + scope : NULL;
    if (!map || !map->count) return NULL;
    size_t low = 0, high = map->sorted;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        const sl_unit *unit = map->units[mid];
        int order = strcmp(unit->unit->family, family);
        if (!order) order = strcmp(unit->unit->name, name);
        if (!order) return map->units[mid];
        if (order < 0) low = mid + 1; else high = mid;
    }
    if (s->reuse.table_scope == scope) return sl_reuse_bound(s, family, name);
    for (size_t i = map->sorted; i < map->count; i++)
        if (!strcmp(map->units[i]->unit->family, family) && !strcmp(map->units[i]->unit->name, name)) return map->units[i];
    return NULL;
}
static int sl_units_push(sl_unit ***items, size_t *count, size_t *capacity, sl_unit *unit)
{
    if (*count == *capacity) {
        size_t next = *capacity ? *capacity * 2 : 64;
        sl_unit **grown = next <= SIZE_MAX / sizeof(*grown) && !SL_SHARE_REFUSED() ? realloc(*items, next * sizeof(*grown)) : NULL;
        if (!grown) return 0;
        *items = grown; *capacity = next;
    }
    (*items)[(*count)++] = unit; return 1;
}
/* A baseline request reaches a shared stock unit no baseline unit reached before: it and every stock unit it reaches
 * become members of the baseline scope, as the baseline's own discovery of it would have made them. Its discovery
 * read nothing the baseline resolves otherwise (sl_fold_local), so that discovery is this one. */
static int sl_env_fail(sh_package_source_link *s, const char *reason, const char *name);
static int sl_promote(sh_package_source_link *s, sl_unit *root)
{
    sl_unit **stack = NULL;
    size_t depth = 0, room = 0;
    root->stock_only = 0; s->share.promoted++;
    if (!sl_units_push(&stack, &depth, &room, root)) return sl_env_fail(s, "cannot promote shared stock", root->unit->name);
    while (depth) {
        sl_unit *unit = stack[--depth];
        for (uint32_t d = 0; unit->discovery && d < unit->discovery->dependency_count; d++) {
            sl_unit *target = unit->discovery->dependencies[d];
            if (!target->stock_only) continue;
            target->stock_only = 0; s->share.promoted++;
            if (!sl_units_push(&stack, &depth, &room, target)) {
                free(stack); return sl_env_fail(s, "cannot promote shared stock", target->unit->name);
            }
        }
    }
    free(stack); return 1;
}
static sl_unit *sl_find(sh_package_source_link *s, size_t scope, const char *family, const char *name, int add)
{
    size_t at;
    if (scope >= s->scope_count || !family || !*family || !name || !*name) {
        sl_fail(s, "resource reference has no scoped identity", name); return NULL;
    }
    if (!s->unit_slot_count || (add && s->count >= s->unit_slot_count / 2))
        if (!sl_unit_rehash(s)) goto bad;
    at = (size_t)sl_unit_hash(scope, family, name) & (s->unit_slot_count - 1);
    while (s->unit_slots[at]) {
        sl_unit *unit = s->unit_slots[at];
        if (unit->scope == scope && !strcmp(unit->unit->family, family) && !strcmp(unit->unit->name, name)) {
            if (add && unit->stock_only && !s->frozen && !sl_promote(s, unit)) return NULL;
            return unit;
        }
        at = (at + 1) & (s->unit_slot_count - 1);
    }
    /* What a package scope binds once in the baseline instead of its own copy. */
    if (scope != s->share.baseline) {
        sl_unit *shared = sl_share_find(s, scope, family, name);
        if (shared) return shared;
        /* Or what a restored scope binds by its record (R-348), when it first requests it. */
        if (add && s->reuse.table_scope == scope && !s->frozen) {
            if ((shared = sl_reuse_bind(s, scope, family, name)) != NULL) return shared;
            if (s->failed) return NULL;
        }
    }
    if (!add || s->frozen) return NULL;
    if (s->count >= UINT32_MAX - 1) goto bad;
    if (!sl_grow((void ***)&s->units, &s->capacity, s->count)) goto bad;
    sl_unit *unit = calloc(1, sizeof(*unit));
    if (!unit) goto bad;
    {
        uint32_t room = 0;
        if (!sl_arena_grow(&s->discovery_arena, (void **)&unit->discovery, &room, 0, sizeof(*unit->discovery))) {
            free(unit); goto bad;
        }
        memset(unit->discovery, 0, sizeof(*unit->discovery));
        unit->unit = &unit->discovery->record;
    }
    unit->unit->family = sl_intern(s, family); unit->unit->name = sl_intern(s, name);
    if (!unit->unit->family || !unit->unit->name) { free(unit); goto bad; }
    unit->scope = (uint32_t)scope; unit->unit->compilation = s->scopes[scope];
    unit->discovery->parent = s->discovering && s->discovering->scope == scope ? s->discovering : NULL;
    if (!unit->discovery->parent) {
        /* Diagnostics only: a failed copy leaves the reason unknown. */
        char note[400];
        if (s->discovering)
            snprintf(note, sizeof(note), "requested from scope %zu by %s %s", (size_t)s->discovering->scope,
                s->discovering->unit->family, s->discovering->unit->name);
        else snprintf(note, sizeof(note), "%s", s->seed_note[0] ? s->seed_note : "unrecorded seed");
        unit->discovery->seed = _strdup(note);
    }
    unit->index = (uint32_t)s->count; s->units[s->count++] = unit; s->unit_slots[at] = unit;
    if (++s->share.live > s->share.peak_live) s->share.peak_live = s->share.live;
    /* Scope by scope, a new unit waits on its scope's pending list (sl_discover_scopes). */
    if (s->share.scope_major) {
        sl_pending *pending = s->share.pending + scope;
        if (!sl_units_push(&pending->units, &pending->count, &pending->capacity, unit)) {
            sl_env_fail(s, "cannot queue a scoped resource identity", name); return NULL;
        }
    }
    return unit;
bad:
    sl_env_fail(s, "cannot retain scoped resource identity", name); return NULL;
}
static int sl_original_rehash(sh_package_source_link *s)
{
    size_t next = s->original_slot_count ? s->original_slot_count * 2 : 64;
    sl_original **slots;
    if (next <= s->original_slot_count || next > SIZE_MAX / sizeof(*slots) || !(slots = calloc(next, sizeof(*slots)))) return 0;
    for (size_t i = 0; i < s->original_count; i++) {
        sl_original *file = s->originals[i];
        size_t at = (size_t)sl_hash_text(0, file->resource.engine_path) & (next - 1);
        while (slots[at]) at = (at + 1) & (next - 1);
        slots[at] = file;
    }
    free(s->original_slots); s->original_slots = slots; s->original_slot_count = next; return 1;
}
static sl_original *sl_original_get(sh_package_source_link *s, const char *path);
static const unsigned char *sl_original_bytes(sh_package_source_link *s, const sl_original *file,
    unsigned char **owned, char *error, size_t capacity);
/* A product output's input and that input's own product inputs, kept as its
 * generated_inputs: canonical paths, each once. */
static int sl_product_input(sh_compiled_resource *resource, const char *path)
{
    char **grown, *copy;
    for (size_t i = 0; i < resource->generated_input_count; i++)
        if (!strcmp(resource->generated_inputs[i], path)) return 1;
    if (resource->generated_input_count >= SIZE_MAX / sizeof(*grown) - 1 || !(copy = _strdup(path))) return 0;
    grown = realloc(resource->generated_inputs, (resource->generated_input_count + 1) * sizeof(*grown));
    if (!grown) { free(copy); return 0; }
    resource->generated_inputs = grown; grown[resource->generated_input_count++] = copy; return 1;
}
static int sl_product_inputs(sh_compiled_resource *resource, const sh_compiled_resource *input)
{
    for (size_t i = 0; input->generated && i < input->generated_input_count; i++)
        if (!sl_product_input(resource, input->generated_inputs[i])) return 0;
    return 1;
}
static void sl_product_clear(sh_compiled_resource *resource)
{
    for (size_t i = 0; i < resource->generated_input_count; i++) free(resource->generated_inputs[i]);
    free(resource->generated_inputs);
    resource->generated_inputs = NULL; resource->generated_input_count = 0;
}
/* scope SIZE_MAX reads the originals; otherwise that scope's supplied files
 * first, as the compiler's view of that scope does. */
typedef struct sl_product_view {
    sh_package_source_link *s; size_t scope; sh_compiled_resource *target; int failed;
} sl_product_view;
static int sl_product_read(void *context, const char *path, unsigned char **body, size_t *length)
{
    sl_product_view *view = context;
    const sh_package_compilation *scope = view->s->scope_count ?
        view->s->scopes[view->scope == SIZE_MAX ? 0 : view->scope] : NULL;
    char *key = !path ? NULL : scope && scope->canonical_path ? scope->canonical_path(path) : sh_package_engine_path(path);
    const sh_compiled_resource *own = key && view->scope != SIZE_MAX ? sl_lookup_supplied(view->s, view->scope, key) : NULL;
    sl_original *input = key && !own ? sl_original_get(view->s, key) : NULL;
    const sh_compiled_resource *read = own ? own : input ? &input->resource : NULL;
    int ok = read && (own || (input->status >= 0 && !input->producing)) &&
        sl_product_input(view->target, key) && sl_product_inputs(view->target, read);
    *body = NULL; *length = 0;
    if (ok && own) {
        char error[256];
        ok = sh_package_owners_union(&view->target->owners, &own->owners) &&
            sh_package_owners_union(&view->target->gameplay_owners, &own->gameplay_owners) &&
            (*body = sh_package_compilation_read(scope, own, SIZE_MAX, length, error, sizeof(error))) != NULL;
    }
    free(key);
    if (!ok) { view->failed = 1; return -1; }
    if (own) return 1;
    if (!input->status) return 0;
    unsigned char *owned = NULL;
    const unsigned char *bytes = sl_original_bytes(view->s, input, &owned, NULL, 0);
    if (!bytes) { view->failed = 1; return -1; }
    if (owned) { *body = owned; *length = input->resource.body_length; return 1; }
    if (!(*body = malloc(input->resource.body_length + 1))) { view->failed = 1; return -1; }
    memcpy(*body, bytes, input->resource.body_length); (*body)[input->resource.body_length] = 0;
    *length = input->resource.body_length; return 1;
}
/* Generate path with the scopes' producer into resource. 0 when it does not
 * claim path, 1 when generated, -1 when a claimed generation failed. */
static int sl_produce(sh_package_source_link *s, size_t scope, const char *path, sh_compiled_resource *resource)
{
    sl_product_view view = {s, scope, resource, 0};
    unsigned char *body = NULL;
    size_t length = 0;
    int claimed = s->native.produce(s->native.producer_context, path, sl_product_read, &view, &body, &length);
    if (!claimed && !view.failed) { free(body); sl_product_clear(resource); return 0; }
    /* Asked only whether the claimed name exists: a producer that cannot
     * generate it from this capture's inputs offers no such resource here. */
    if (claimed < 0 && !view.failed && s->product_optional) { free(body); sl_product_clear(resource); return 0; }
    if (claimed < 0 || view.failed || !body || length == SIZE_MAX) {
        free(body); sl_product_clear(resource);
        if (!s->failed) sl_fail(s, "product resource cannot be generated from its inputs", path);
        return -1;
    }
    resource->body = body; resource->body_length = length; resource->generated = 1; return 1;
}
/* An absent original the scopes' producer claims is its output over the
 * originals, original scope 3, exactly as the compiler evaluates an absent
 * original (pc_build_original). Unclaimed, it stays absent. */
static int sl_product(sh_package_source_link *s, sl_original *file)
{
    int status;
    file->producing = 1;
    status = sl_produce(s, SIZE_MAX, file->resource.engine_path, &file->resource);
    file->producing = 0;
    if (status < 0) { file->status = -1; return 0; }
    if (status) file->status = 3;
    return 1;
}
/* Whether a scope serves every input a product original read as that original. */
static int sl_product_unchanged(sh_package_source_link *s, size_t scope, const sl_original *file)
{
    for (size_t i = 0; i < file->resource.generated_input_count; i++) {
        const sh_compiled_resource *own = sl_lookup_supplied(s, scope, file->resource.generated_inputs[i]);
        if (own && sh_package_owners_count(&own->owners)) return 0;
    }
    return 1;
}
/* Where a scope changes an input a product original read, the scope's own
 * generated resource: the producer over that scope's files and the originals,
 * as its compiler view evaluates it. Owned by the capture, one per scope and
 * path; its inputs' owners own it. NULL with the failure recorded. */
static const sh_compiled_resource *sl_scope_product(sh_package_source_link *s, size_t scope, const sl_original *original)
{
    sh_compiled_resource *resource;
    for (size_t i = 0; i < s->scope_product_count; i++)
        if (s->scope_products[i].scope == scope &&
            !strcmp(s->scope_products[i].resource->engine_path, original->resource.engine_path))
            return s->scope_products[i].resource;
    if (s->frozen) { sl_env_fail(s, "source binding requested an uncaptured scope product", original->resource.engine_path); return NULL; }
    if (s->scope_product_count == s->scope_product_capacity) {
        size_t next = s->scope_product_capacity ? s->scope_product_capacity * 2 : 4;
        struct sl_scope_product *grown = next <= SIZE_MAX / sizeof(*grown) ?
            realloc(s->scope_products, next * sizeof(*grown)) : NULL;
        if (!grown) { sl_env_fail(s, "cannot retain a scope's product resource", original->resource.engine_path); return NULL; }
        s->scope_products = grown; s->scope_product_capacity = next;
    }
    if (!(resource = calloc(1, sizeof(*resource))) || !(resource->engine_path = _strdup(original->resource.engine_path))) {
        free(resource); sl_env_fail(s, "cannot retain a scope's product resource", original->resource.engine_path); return NULL;
    }
    resource->owns_engine_path = 1; resource->source = SIZE_MAX; resource->baseline_known = 3;
    if (sl_produce(s, scope, resource->engine_path, resource) != 1) {
        if (!s->failed) sl_fail(s, "a scope's product resource is no longer claimed", resource->engine_path);
        sh_package_owners_free(&resource->owners); sh_package_owners_free(&resource->gameplay_owners);
        free((char *)resource->engine_path); free(resource); return NULL;
    }
    s->scope_products[s->scope_product_count++] = (struct sl_scope_product){scope, resource};
    return resource;
}
static sl_original *sl_original_get(sh_package_source_link *s, const char *path)
{
    size_t at;
    if (!s->original_slot_count || (!s->frozen && s->original_count >= s->original_slot_count / 2))
        if (!sl_original_rehash(s)) goto bad;
    at = (size_t)sl_hash_text(0, path) & (s->original_slot_count - 1);
    while (s->original_slots[at]) {
        sl_original *file = s->original_slots[at];
        if (!strcmp(file->resource.engine_path, path)) {
            if (file->released) {
                /* The same open catalog serves the same bytes again. */
                unsigned char *body = NULL; size_t length = 0;
                int status = s->original ? s->original(s->original_context, path, &body, &length) : -1;
                sh_package_file_identity identity;
                if (status != file->status || !body || (file->identified &&
                    (!sh_package_bytes_identity(body,length,&identity) || identity.length != file->identity.length ||
                     memcmp(identity.digest,file->identity.digest,sizeof(identity.digest))))) {
                    free(body); sl_env_fail(s, "released original could not be read again", path); return NULL;
                }
                file->resource.body = body; file->resource.body_length = length; file->released = 0;
            }
            return file;
        }
        at = (at + 1) & (s->original_slot_count - 1);
    }
    if (s->frozen) { sl_env_fail(s, "source binding requested an uncaptured original", path); return NULL; }
    if (!sl_grow((void ***)&s->originals, &s->original_capacity, s->original_count)) goto bad;
    sl_original *file = calloc(1, sizeof(*file));
    if (!file) goto bad;
    file->resource.engine_path = _strdup(path); file->resource.source = SIZE_MAX;
    if (!file->resource.engine_path) { free(file); goto bad; }
    unsigned long long reading = sl_now_us();
    if (s->native.original_view && SL_BORROW) {
        const unsigned char *view = NULL;
        sh_package_body *owner = NULL;
        file->status = s->native.original_view(s->native.original_view_context, path, &view, &file->resource.body_length,
            &owner);
        file->resource.body = (unsigned char *)view; file->borrowed = view != NULL;
        /* Bytes the reader holds as a shared body: this original takes a reference, and what serves it shares them. */
        if (view && owner && view == sh_package_body_bytes(owner) && file->resource.body_length == sh_package_body_length(owner))
            file->resource.body_owner = sh_package_body_retain(owner);
    } else file->status = s->original ? s->original(s->original_context, path,
        &file->resource.body, &file->resource.body_length) : 0;
    s->census.original_us += sl_now_us() - reading;
    s->census.originals++;
    if (file->status > 0 && file->resource.body) s->census.original_bytes += file->resource.body_length;
    s->originals[s->original_count++] = file; s->original_slots[at] = file;
    if (file->status < 0 || file->status > 3 || (file->status && !file->resource.body) ||
        file->resource.body_length == SIZE_MAX) {
        file->status = -1; sl_env_fail(s, "original resource could not be captured", path);
    } else if (!file->status) {
        if (file->resource.body_owner) sh_package_body_release(file->resource.body_owner);
        else if (!file->borrowed) free(file->resource.body);
        file->resource.body_owner = NULL;
        file->borrowed = 0; file->resource.body = NULL; file->resource.body_length = 0;
        if (s->native.produce) sl_product(s, file);
    }
    file->resource.baseline_known = file->status > 0 ? file->status : 0;
    file->resource.native_original = file->status == 1;
    return file;
bad:
    sl_env_fail(s, "cannot allocate original resource snapshot", path); return NULL;
}
/* The captured original of path, without capturing one. */
static sl_original *sl_original_peek(const sh_package_source_link *s, const char *path)
{
    if (!s->original_slot_count || !path) return NULL;
    size_t at = (size_t)sl_hash_text(0, path) & (s->original_slot_count - 1);
    while (s->original_slots[at]) {
        sl_original *file = s->original_slots[at];
        if (!strcmp(file->resource.engine_path, path)) return file;
        at = (at + 1) & (s->original_slot_count - 1);
    }
    return NULL;
}
/* A captured original that holds its shared body without its bytes (sl_originals_release): it reads them through
 * the body, which reads them again from its backing when every holder let them go (R-342). */
static int sl_original_detached(const sl_original *file)
{ return !file->resource.body && file->resource.body_owner; }
/* The captured original's bytes for one read: its own while it holds them, else read again through its body into
 * *owned, which the caller frees. NULL when they cannot be read exactly (the body records it): an environment
 * failure, reported to error when given, else to the capture. */
static const unsigned char *sl_original_bytes(sh_package_source_link *s, const sl_original *file,
    unsigned char **owned, char *error, size_t capacity)
{
    size_t length = 0;
    *owned = NULL;
    if (!sl_original_detached(file)) return file->resource.body;
    unsigned long long began = sl_now_us();
    *owned = sh_package_body_read(file->resource.body_owner, &length);
    s->share.rereads++; s->share.reread_us += sl_now_us() - began;
    if (*owned) s->share.reread_bytes += length;
    if (*owned && length == file->resource.body_length) return *owned;
    free(*owned); *owned = NULL;
    if (error && capacity)
        snprintf(error, capacity, "captured original could not be read again: %s", file->resource.engine_path);
    else sl_env_fail(s, "captured original could not be read again", file->resource.engine_path);
    return NULL;
}
/* A release point (R-342): every captured original whose shared body has a backing stops holding its bytes, then
 * the snapshot behind the view does the same (sh_package_source_link_native.originals_released). */
static void sl_originals_release(sh_package_source_link *s)
{
    for (size_t i = 0; i < s->original_count; i++) {
        sl_original *file = s->originals[i];
        if (!file->resource.body || !file->resource.body_owner || !sh_package_body_backed(file->resource.body_owner))
            continue;
        file->resource.body = NULL; sh_package_body_detach(file->resource.body_owner);
    }
    if (s->native.originals_released) s->native.originals_released(s->native.originals_released_context);
}
/* The borrowing form of sl_read_original: the capture's own bytes, valid until it closes or its next release
 * point. A detached original holds them again (read again when they were let go). */
static int sl_view_original(void *context, const char *path, const unsigned char **body, size_t *length,
    sh_package_body **owner)
{
    sh_package_source_link *s = context;
    sl_original *file = sl_original_get(s, path);
    *body = NULL; *length = 0; *owner = NULL;
    if (!file || file->status < 0) return -1;
    if (!file->status) return 0;
    if (sl_original_detached(file)) {
        unsigned long long began = sl_now_us();
        file->resource.body = (unsigned char *)sh_package_body_attach(file->resource.body_owner);
        s->share.rereads++; s->share.reread_us += sl_now_us() - began;
        if (!file->resource.body) { sl_env_fail(s, "captured original could not be read again", path); return -1; }
        s->share.reread_bytes += file->resource.body_length;
    }
    *body = file->resource.body; *length = file->resource.body_length; *owner = file->resource.body_owner;
    return file->status;
}
static int sl_read_original(void *context, const char *path, unsigned char **body, size_t *length)
{
    sh_package_source_link *s = context;
    sl_original *file = sl_original_get(s, path);
    *body = NULL; *length = 0;
    if (!file || file->status < 0) return -1;
    if (!file->status) return 0;
    if (sl_original_detached(file)) {
        /* Its bytes read again through the shared body: already this caller's copy. */
        unsigned char *owned = NULL;
        if (!sl_original_bytes(s, file, &owned, NULL, 0)) return -1;
        *body = owned; *length = file->resource.body_length; return file->status;
    }
    *body = malloc(file->resource.body_length + 1);
    if (!*body) { sl_env_fail(s, "cannot copy captured original", path); return -1; }
    memcpy(*body, file->resource.body, file->resource.body_length); (*body)[file->resource.body_length] = 0;
    *length = file->resource.body_length; return file->status;
}

static int sl_read_original_observed(void *context, const char *path, unsigned char **body, size_t *length)
{
    sh_package_source_link *s = context;
    if (sl_original_peek(s, path) || !s->original) return sl_read_original(s, path, body, length);
    *body = NULL; *length = 0;
    int status = s->original(s->original_context, path, body, length);
    if (status <= 0 || status > 3 || !*body) {
        free(*body); *body = NULL; *length = 0;
        return status < 0 || status > 3 || (status && !*body) ? -1 : 0;
    }
    return status;
}

#define SL_OBSERVED_KEEP (256u * 1024u)
static void sl_observed_release(sh_package_source_link *base, const sl_unit *unit)
{
    for (size_t i = 0; i < unit->unit->file_count; i++) {
        sl_original *original = sl_original_peek(base, unit->unit->files[i]->engine_path);
        if (!original || &original->resource != unit->unit->files[i] || !original->resource.body ||
            original->resource.body_length < SL_OBSERVED_KEEP || original->status <= 0) continue;
        if (original->resource.body_owner) {
            if (sh_package_body_backed(original->resource.body_owner)) {
                original->resource.body = NULL; sh_package_body_detach(original->resource.body_owner);
            }
            continue;
        }
        if (original->borrowed) continue;
        if (!original->identified) {
            if (!sh_package_bytes_identity(original->resource.body,original->resource.body_length,&original->identity)) continue;
            original->identified = 1;
        }
        free(original->resource.body); original->resource.body = NULL; original->resource.body_length = 0;
        original->released = 1;
    }
}
/* sl_read_original's answer without its bytes (sh_package_source_reader_original_status). */
static int sl_original_status(void *context, const char *path)
{
    SL_STATUS_QUERY();
    sl_original *file = sl_original_get(context, path);
    return !file || file->status < 0 ? -1 : file->status;
}
/* Diagnostics only: keep the first reason a unit's binding is not original. */
static void sl_note_original(sl_unit *unit, const char *note)
{
    sl_extra *extra = sl_extra_of(unit);
    if (extra && !extra->original_note) extra->original_note = _strdup(note);
}
static int sl_member(sh_package_source_link *s, sl_unit *unit, const sh_compiled_resource *file)
{
    for (size_t i = 0; i < unit->unit->file_count; i++)
        if (!strcmp(unit->unit->files[i]->engine_path, file->engine_path)) return 1;
    size_t room = unit->discovery->capacity;
    if (!sl_grow_unit((void ***)&unit->unit->files, &room, unit->unit->file_count) || room > UINT32_MAX ||
        !(unit->discovery->capacity = (uint32_t)room))
        return sl_env_fail(s, "cannot retain native resource permutations", unit->unit->name);
    ((const sh_compiled_resource **)unit->unit->files)[unit->unit->file_count++] = file; return 1;
}
/* Capture availability separately from membership. Optional consumers may
 * omit a campaign-only original, but never substitute a peer package or turn
 * an I/O failure into absence. The original snapshot retains either result.
 * optional 2 is a stock-route unit's own file (sl_path): a campaign-only
 * original returns 2, its boundary, and nothing is captured for it. */
static int sl_source_file(sh_package_source_link *s, size_t scope_index,
    const char *path, int optional, const sh_compiled_resource **out)
{
    const sh_package_compilation *scope = s->scopes[scope_index];
    char *key = scope->canonical_path ? scope->canonical_path(path) : sh_package_engine_path(path);
    const sh_compiled_resource *file;
    int result = -1;
    *out = NULL;
    if (!key) { sl_fail(s, "invalid native dependency path", path); return -1; }
    file = sl_lookup_supplied(s, scope_index, key);
    if (file) { *out = file; result = 1; }
    else {
        sl_original *original = sl_original_get(s, key);
        if (original && original->status == 2) {
            /* Campaign-only data is absent from SnapMap. Reverse observation
             * treats that like any proven absence: a boundary, not admission. */
            if (optional == 2 && !s->observe) result = 2;
            else if (optional || s->observe) result = 0;
            else sl_fail(s, "campaign dependency is absent from its authored scope", key);
        } else if (original && original->status > 0) {
            if (!original->resource.generated || sl_product_unchanged(s, scope_index, original)) {
                *out = &original->resource; result = 1;
            } else if ((*out = sl_scope_product(s, scope_index, original)) != NULL) result = 1;
        } else if (original && !original->status) result = 0;
    }
    free(key); return result;
}
/* 1 supplied, 0 proven absent, -1 failed. No peer compilation is consulted.
 * For a unit's own catalog rows or declaration path (route), a unit no
 * authored state names reads a campaign-only original as the stock route's
 * own absence (sl_authored_mark): 0, with campaign_boundary set. Shader
 * companions (sl_shader_collect_path) keep refusing such an original. */
static int sl_path(sh_package_source_link *s, sl_unit *unit, const char *path, int route)
{
    const sh_compiled_resource *file;
    int stock = route && !s->observe && !unit->authored_request && !unit->unit->file_count;
    int status = sl_source_file(s, unit->scope, path, stock ? 2 : 0, &file);
    if (status == 2) { unit->campaign_boundary = 1; return 0; }
    return status == 1 ? (sl_member(s, unit, file) ? 1 : -1) : status;
}
static char *sl_decl_path(const char *family, const char *name)
{
    size_t a = strlen(family), b = strlen(name);
    char *path;
    if (a > SIZE_MAX - 24 || b > SIZE_MAX - 24 - a || !(path = malloc(a + b + 24))) return NULL;
    snprintf(path, a + b + 24, "generated/decls/%s/%s.decl", family, name); return path;
}
static size_t sl_shader_program_add(sh_package_source_link *s, sl_unit *unit,
    const char *name, size_t parent, uint64_t mask)
{
    for (size_t i = 0; i < SL_EXTRA(unit)->program_count; i++) if (!strcmp(unit->extra->programs[i].name, name)) return i;
    sl_extra *extra = sl_extra_of(unit);
    if (!extra) { sl_env_fail(s, "cannot retain shader permutation identities", name); return SIZE_MAX; }
    if (extra->program_count >= SIZE_MAX / sizeof(*extra->programs)) return SIZE_MAX;
    char *copy = _strdup(name);
    sl_shader_program *grown = copy ? realloc(extra->programs, (extra->program_count + 1) * sizeof(*grown)) : NULL;
    if (!grown) { free(copy); sl_env_fail(s, "cannot retain shader permutation identities", name); return SIZE_MAX; }
    extra->programs = grown;
    extra->programs[extra->program_count] = (sl_shader_program){copy, parent, mask, {0}};
    return extra->program_count++;
}
typedef struct sl_shader_collect {
    sh_package_source_link *source;
    sl_unit *unit;
    size_t program, permutation;
    const sh_package_shader_program *decoded;
    const unsigned char *binary;
    int probe, declaration, cooked;
} sl_shader_collect;
static int sl_shader_collect_path(void *context, sh_package_shader_path_kind kind,
    const char *original, const char *linked)
{
    sl_shader_collect *walk = context; sh_package_source_link *s = walk->source; sl_unit *unit = walk->unit;
    (void)linked;
    if (kind == SH_PACKAGE_SHADER_PERMUTATION) {
        if (walk->permutation >= walk->decoded->permutation_count) return 0;
        return sl_shader_program_add(s, unit, original, walk->program,
            walk->decoded->permutations[walk->permutation++]) != SIZE_MAX;
    }
    int status = sl_path(s, unit, original, 0);
    if (walk->probe) {
        if (kind == SH_PACKAGE_SHADER_DECLARATION) walk->declaration = status;
        if (kind == SH_PACKAGE_SHADER_BINARY) walk->cooked = status;
        return status >= 0;
    }
    if (status != 1) return status < 0 ? 0 : sl_gap_fail(s, "required shader companion is absent", original);
    const sh_compiled_resource *file = NULL;
    for (size_t i = 0; i < unit->unit->file_count; i++) if (!strcmp(unit->unit->files[i]->engine_path, original)) {
        file = unit->unit->files[i]; break;
    }
    if (!file) return sl_fail(s, "shader companion was not retained", original);
    if (kind == SH_PACKAGE_SHADER_SPIRV) {
        size_t length = 0; unsigned char *body = sh_package_compilation_read(unit->unit->compilation, file,
            SIZE_MAX, &length, s->error, s->error_capacity);
        const char *extension = strrchr(original, '.');
        size_t stage = extension && !strcmp(extension, ".vspv") ? 0 :
            extension && !strcmp(extension, ".fspv") ? 1 : 2;
        const sh_package_shader_stage *source = walk->decoded->stages + stage;
        int ok = body && sh_package_shader_spirv(body, length, walk->binary + source->offset,
            source->length, s->error, s->error_capacity) == SH_PACKAGE_SHADER_OK;
        free(body); if (!ok) return 0;
    }
    for (size_t i = 0; i < SL_EXTRA(unit)->shader_file_count; i++) if (unit->extra->shader_files[i].file == file) return 1;
    sl_extra *extra = sl_extra_of(unit);
    if (!extra) return sl_env_fail(s, "cannot retain shader file roles", original);
    if (extra->shader_file_count >= SIZE_MAX / sizeof(*extra->shader_files)) return 0;
    sl_shader_file *grown = realloc(extra->shader_files, (extra->shader_file_count + 1) * sizeof(*grown));
    if (!grown) return sl_env_fail(s, "cannot retain shader file roles", original);
    extra->shader_files = grown;
    extra->shader_files[extra->shader_file_count++] = (sl_shader_file){file, walk->program, kind}; return 1;
}
static int sl_shader_files(sh_package_source_link *s, sl_unit *unit)
{
    sh_package_shader_program empty = {0};
    sl_shader_collect probe = {s, unit, 0, 0, &empty, NULL, 1, 0, 0};
    if (!s->native.shader) return sl_env_fail(s, "shader unit requires the current renderer and fixed ABI contract", unit->unit->name);
    /* The selected renderer's companions define publication. Supplied files for
     * other renderers remain in their intact source bundle, never substituted. */
    unit->unit->file_count = 0;
    if (sh_package_shader_paths(s->native.shader->renderer, unit->unit->name, unit->unit->name, &empty,
        sl_shader_collect_path, &probe, s->error, s->error_capacity) != SH_PACKAGE_SHADER_OK) return 0;
    if (!probe.declaration && !probe.cooked) {
        /* A generic declaration pointer offers every manager as a candidate;
         * like an absent declaration, an absent program is only a boundary. */
        if (s->observe) return 1;
        int status = s->native.admit ? s->native.admit(s->native.context, unit->scope,
            unit->unit->family, unit->unit->name, unit->unit->semantics, s->error, s->error_capacity) : 0;
        if (status != 1) return sl_gap_fail(s, "source-free shader has no verified native admission", unit->unit->name);
        unit->unit->original_binding = 1; return 1;
    }
    if (probe.declaration == 1 && !probe.cooked && s->native.shader->program) {
        unsigned char *declaration = NULL, *binary = NULL; size_t declaration_length = 0, binary_length = 0;
        sh_package_shader_program decoded = {0};
        sh_package_source_shader_status status = sh_package_source_shader_open(s->readers[unit->scope], unit->unit->name,
            &declaration, &declaration_length, &binary, &binary_length, &decoded, s->error, s->error_capacity);
        free(declaration); free(binary); sh_package_shader_program_clear(&decoded);
        if (status == SH_PACKAGE_SOURCE_SHADER_PREPARING) return sl_defer_preparation(s);
    }
    if (probe.declaration != 1 || probe.cooked != 1)
        return sl_gap_fail(s, "shader declaration and selected renderer binary must both be supplied", unit->unit->name);
    if (sl_shader_program_add(s, unit, unit->unit->name, SIZE_MAX, 0) == SIZE_MAX) return 0;
    /* Once a program awaits native preparation, the unit's remaining programs
     * still queue their own needs for the same round. Any other failure is
     * judged afresh after it; this incomplete unit is never certified. */
    int pending = 0;
    for (size_t p = 0; p < SL_EXTRA(unit)->program_count; p++) {
        unsigned char *declaration = NULL, *binary = NULL; size_t declaration_length = 0, binary_length = 0;
        sh_package_shader_program decoded = {0};
        const char *name = unit->extra->programs[p].name;
        sh_package_source_shader_status status = sh_package_source_shader_open(s->readers[unit->scope], name,
            &declaration, &declaration_length, &binary, &binary_length, &decoded, s->error, s->error_capacity);
        if (status == SH_PACKAGE_SOURCE_SHADER_PREPARING) {
            if (!pending) sl_defer_preparation(s);
            else if (s->error && s->error_capacity) *s->error = 0;
            pending = 1; continue;
        }
        if (status != SH_PACKAGE_SOURCE_SHADER_READY) {
            if (!pending) return 0;
            if (s->error && s->error_capacity) *s->error = 0;
            break;
        }
        sl_shader_collect walk = {s, unit, p, 0, &decoded, binary, 0, 0, 0};
        int ok = sh_package_shader_paths(s->native.shader->renderer, name, name, &decoded,
            sl_shader_collect_path, &walk, s->error, s->error_capacity) == SH_PACKAGE_SHADER_OK;
        free(declaration); free(binary);
        if (!ok) {
            sh_package_shader_program_clear(&decoded);
            if (!pending || s->failed || s->environment) return 0;
            if (s->error && s->error_capacity) *s->error = 0;
            break;
        }
        unit->extra->programs[p].decoded = decoded;
    }
    return pending ? sl_defer_preparation(s) : 1;
}
typedef struct sl_material_members { sh_package_source_link *source; sl_unit *unit; } sl_material_members;
static int sl_material_member(void *context, const char *role, const sh_compiled_resource *file)
{
    sl_material_members *walk = context; sh_package_link_unit *unit = walk->unit->unit;
    if (!role || !*role || !file || !file->engine_path ||
        unit->witness_count >= SIZE_MAX / sizeof(*unit->witnesses))
        return sl_fail(walk->source, "invalid material companion witness", role);
    for (size_t i = 0; i < unit->witness_count; i++) if (!strcmp(role, unit->witnesses[i].role))
        return sl_fail(walk->source, "material companion repeats a native role", role);
    char *copy = _strdup(role);
    sh_package_link_witness *grown = copy ? realloc((void *)unit->witnesses,
        (unit->witness_count + 1) * sizeof(*grown)) : NULL;
    if (!grown) { free(copy); return sl_env_fail(walk->source, "cannot retain material companions", role); }
    unit->witnesses = grown; grown[unit->witness_count++] = (sh_package_link_witness){copy, file}; return 1;
}
static int sl_movie_files(sh_package_source_link *s, sl_unit *unit);
static unsigned char *sl_movie_bind(sl_unit *unit, const sh_compiled_resource *file,
    const unsigned char *body, size_t length, sh_package_binding_resolver reference, void *context,
    size_t *output_length, char *error, size_t capacity);
static int sl_files(sh_package_source_link *s, sl_unit *unit)
{
    const sh_resource_catalog_entry *const *rows = NULL;
    size_t count;
    if (!strcmp(unit->unit->family, "renderprog")) return sl_shader_files(s, unit);
    if (sh_package_movie_owner(unit->unit->family, unit->unit->name)) return sl_movie_files(s, unit);
    if (!strcmp(unit->unit->family, "font")) {
        /* Native selection is between these forms, including an empty selected
         * file's 12.dat fallback. Catalog order is not a selection policy. */
        for (unsigned i = 0; i < 3; i++) {
            sh_font_binary_paths paths;
            const sh_compiled_resource *file;
            if (!sh_font_binary_paths_for(unit->unit->name, (sh_font_binary_variant)i, &paths))
                return sl_env_fail(s, "cannot construct font source paths", unit->unit->name);
            int status = sl_source_file(s, unit->scope, paths.file, 1, &file);
            sh_font_binary_paths_clear(&paths);
            if (status < 0 || (status == 1 && !sl_member(s, unit, file))) return 0;
        }
        return unit->unit->file_count || s->observe || sl_fail(s, "font has no captured glyph source", unit->unit->name);
    }
    /* Supplied declarations already have a verified source identity. Native
     * cooked cache paths are not additional declaration text envelopes. */
    if (unit->unit->file_count && unit->unit->files[0]->type) goto companions;
    count = sh_resource_catalog_find(s->catalog, unit->unit->family, unit->unit->name, &rows);
    if (count) {
        for (size_t i = 0; i < count; i++) if (*rows[i]->path) {
            int status = sl_path(s, unit, rows[i]->path, 1);
            if (status != 1 && !((s->observe || unit->campaign_boundary) && status == 0))
                return status < 0 ? 0 : sl_fail(s, "native resource permutation is absent", rows[i]->path);
        }
    } else if (!unit->unit->file_count) {
        char *path = sh_package_cooked_path(unit->unit->family, unit->unit->name);
        if (!path) path = sl_decl_path(unit->unit->family, unit->unit->name);
        if (!path) return sl_env_fail(s, "cannot construct native source path", unit->unit->name);
        int status = sl_path(s, unit, path, 1); free(path);
        if (status < 0) return 0;
    }
    if (unit->campaign_boundary) {
        /* The stock route's own campaign-only reference (sl_authored_mark): its
         * registered name is the whole identity; nothing is delivered. A unit
         * that SnapMap carries only in part stays a campaign dependency. */
        if (unit->unit->file_count)
            return sl_fail(s, "campaign dependency is absent from its authored scope", unit->unit->name);
        char identity[512]; sh_package_file_identity digest;
        int n = snprintf(identity, sizeof(identity), "doom2016:snapmap-absent-campaign-original:%s:%s",
            unit->unit->family, unit->unit->name);
        if (n <= 0 || (size_t)n >= sizeof(identity) || !sh_package_bytes_identity(identity, (size_t)n, &digest))
            return sl_env_fail(s, "cannot identify a campaign-only stock reference", unit->unit->name);
        memcpy(unit->unit->semantics, digest.digest, 32);
        unit->unit->original_binding = 1; s->census.campaign_boundaries++;
        return 1;
    }
    if (!unit->unit->file_count) {
        if (s->observe) return 1;
        /* SnapMap registers some identities with no data. Loading one reports
         * "No data" through the non-fatal load reporter and defaults it (MD6
         * anim, skeleton and mesh loaders, Vulkan 0x1414865e0 / 0x1414ab4b0 /
         * 0x14149c950); stock declarations name such rows and load normally.
         * The registered name is the whole identity: nothing to deliver. */
        int registered = 0, status;
        for (size_t i = 0; i < count; i++) registered |= !*rows[i]->path && rows[i]->archive < 2;
        if (registered) {
            char identity[512]; sh_package_file_identity digest;
            int n = snprintf(identity, sizeof(identity), "doom2016:engine-registered-fileless:%s:%s",
                unit->unit->family, unit->unit->name);
            status = n > 0 && (size_t)n < sizeof(identity) &&
                sh_package_bytes_identity(identity, (size_t)n, &digest) ? 1 : -1;
            if (status == 1) memcpy(unit->unit->semantics, digest.digest, 32);
        } else status = s->native.admit ? s->native.admit(s->native.context, unit->scope,
            unit->unit->family, unit->unit->name, unit->unit->semantics, s->error, s->error_capacity) : 0;
        if (status != 1) return sl_fail(s, "resource has no admitted native source", unit->unit->name);
        unit->unit->original_binding = 1;
    }
companions:
    if (unit->unit->file_count && !unit->discovery->material_members && !strcmp(unit->unit->family, "material") && s->native.material_companions) {
        sl_material_members walk = {s, unit};
        unsigned long long asking = sl_now_us();
        int ok = s->native.material_companions(s->native.context, unit->scope, unit->unit->name,
            sl_material_member, &walk, s->error, s->error_capacity);
        s->census.companion_us += sl_now_us() - asking;
        if (ok) unit->discovery->material_members = 1;
        return ok;
    }
    return 1;
}

static unsigned char *sl_bind_member(sh_package_source_link *s, sl_unit *unit,
    const sh_compiled_resource *file, const unsigned char *body, size_t length,
    sh_package_binding_resolver reference, void *context, size_t *output_length,
    char *error, size_t capacity)
{
    if (sh_package_movie_owner(unit->unit->family, unit->unit->name))
        return sl_movie_bind(unit, file, body, length, reference, context, output_length, error, capacity);
    if (!strcmp(unit->unit->family, "font")) {
        /* Name-derived references belong to the complete constructor plan,
         * not to offsets in glyph bytes. Empty members are native fallbacks. */
        if ((length && !sh_font_binary_read(body, length, NULL, error, capacity)) || length == SIZE_MAX) return NULL;
        unsigned char *out = malloc(length + 1);
        if (out) { memcpy(out, body, length); out[length] = 0; *output_length = length; }
        return out;
    }
    if (!strcmp(unit->unit->family, "renderprog")) {
        for (size_t i = 0; i < SL_EXTRA(unit)->shader_file_count; i++) {
            const sl_shader_file *member = unit->extra->shader_files + i;
            if (member->file != file) continue;
            if (member->kind == SH_PACKAGE_SHADER_SPIRV ||
                member->kind == SH_PACKAGE_SHADER_SOURCE_INCLUDE) {
                /* Discovery already validated this companion against its
                 * captured program. It has no relocatable reference sites.
                 * Verify the exact retained member rather than reopening,
                 * decoding and checking every companion of the whole program
                 * once for each stage/include on every identity pass. */
                size_t expected_length = 0;
                unsigned char *expected = sh_package_compilation_read(unit->unit->compilation,
                    file, SIZE_MAX, &expected_length, error, capacity);
                if (!expected) return NULL;
                if (length != expected_length || memcmp(body, expected, length)) {
                    free(expected);
                    snprintf(error, capacity, "shader companion differs from its captured source unit");
                    return NULL;
                }
                *output_length = expected_length; return expected;
            }
            else
                return sh_package_source_shader_bind(s->readers[unit->scope],
                    unit->extra->programs[member->program].name, file->engine_path,
                    body, length, reference, context, output_length, error, capacity);
        }
        snprintf(error, capacity, "file is outside its captured shader unit"); return NULL;
    }
    sh_package_source_binding result;
    unsigned char *out = (s->observe ? sh_package_source_observe : sh_package_source_bind)(s->readers[unit->scope],
        unit->unit->family, unit->unit->name, body, length, reference, context, output_length, &result, error, capacity);
    if (!out && result.malformed) s->malformed = 1;
    if (!out && result.environment) s->environment = 1;
    if (out && result.coverage) unit->coverage = 1;
    return out;
}

/* Physical shader paths supply seeds; the selected renderer's verified reader
 * later accounts for all companions. Catalog family labels cannot substitute
 * for paths: some renderparm files are catalogued beneath their owning shader. */
static int sl_shader_seed(const sh_compiled_resource *file, char **name)
{
    const char *path = file->engine_path, *start = NULL; size_t length = strlen(path), suffix = 0;
    *name = NULL;
    if (!strncmp(path, "generated/decls/renderprog/", 27) && length > 32 && !strcmp(path + length - 5, ".decl")) {
        start = path + 27; suffix = 5;
    } else if (!strncmp(path, "generated/renderprogs/", 22)) {
        if (length > 14 && !strcmp(path + length - 14, "_pc_vulkan.bin")) suffix = 14;
        else if (length > 7 && !strcmp(path + length - 7, "_pc.bin")) suffix = 7;
        if (suffix && strncmp(path + 22, "permutations/", 13)) start = path + 22;
        else return 1;
    } else if (!strncmp(path, "generated/spirv/", 16) || !strncmp(path, "decls/renderprogs/", 18) ||
        !strncmp(path, "generated/glsl/", 15) || !strncmp(path, "generated/glslbinaries/", 23)) return 1;
    else return 0;
    size_t n = length - (size_t)(start - path) - suffix;
    if (!n || !(*name = malloc(n + 1))) return -1;
    memcpy(*name, start, n); (*name)[n] = 0; return 1;
}
static size_t sl_font_stem(const char *path)
{
    static const char *const suffixes[] = {"/48.dat", "/64_df.dat", "/12.dat"};
    if (strncmp(path, "fonts/", 6)) return 0;
    size_t n = strlen(path);
    for (unsigned i = 0; i < 3; i++) {
        size_t suffix = strlen(suffixes[i]);
        if (n > 6 + suffix && !strcmp(path + n - suffix, suffixes[i])) return n - 6 - suffix;
    }
    return 0;
}
static const char *sl_deferred_binary_family(sh_package_source_link *s, const sh_compiled_resource *file)
{
    const sh_resource_catalog_entry *const *rows = NULL;
    size_t n = strlen(file->engine_path);
    if (file->type || n <= 7 || sh_package_generated_family(file, s->catalog) ||
        sh_resource_catalog_find_path(s->catalog, file->engine_path, &rows)) return NULL;
    if (sl_font_stem(file->engine_path)) return "font";
    if (!strcmp(file->engine_path + n - 7, ".bimage"))
        return !strncmp(file->engine_path, "generated/transsortatlas/", 25) ? "transsortatlas" : "image";
    if (!strcmp(file->engine_path + n - 7, ".dmodel")) return "discreteanimation";
    if (n >= 17 && !strncmp(file->engine_path, "generated/", 10) &&
        !strcmp(file->engine_path + n - 7, ".pmodel")) return "staticparticlemodel";
    return NULL;
}
typedef struct sl_material_seed { sh_package_source_link *source; size_t scope; } sl_material_seed;
static int sl_seed_family(sl_material_seed *walk, const char *family, const char *name)
{
    sl_unit *unit = sl_find(walk->source, walk->scope, family, name, 1);
    if (unit && sl_files(walk->source, unit)) return 1;
    if (unit) walk->source->failed_unit = unit;
    return 0;
}
/* A table row that neither the scope nor SnapMap declares as a material (an
 * atlas-only rectangle, which another material applies through virtualmapping,
 * or a campaign-only material) names no SnapMap material object: no SnapMap
 * consumer selects it, so it seeds nothing. Without a catalog every row seeds. */
static int sl_seed_material(void *context, const char *name)
{
    sl_material_seed *walk = context;
    sh_package_source_link *s = walk->source;
    if (s->catalog) {
        const sh_resource_catalog_entry *const *rows = NULL;
        char *path = sl_decl_path("material", name);
        if (!path) return sl_env_fail(s, "cannot name a virtual material seed", name);
        int declared = sh_package_compilation_find(s->scopes[walk->scope], path) != NULL;
        size_t count = declared ? 0 : sh_resource_catalog_find(s->catalog, "material", name, &rows);
        for (size_t i = 0; !declared && i < count; i++) declared = rows[i]->archive < 2;
        if (!declared && !count) {
            /* No catalog row: only an original the baseline itself supplies declares it. */
            sl_original *original = sl_original_get(s, path);
            if (!original || original->status < 0) { free(path); return 0; }
            declared = original->status == 1;
        }
        free(path);
        if (!declared) return 1;
    }
    return sl_seed_family(context, "material", name);
}
static int sl_seed_sound(void *context, const char *name)
{ return sl_seed_family(context, "sound", name); }
/* A byte-identical SnapMap include contributes no change to catalogued programs.
 * Keep it in the authored delivery and bind it normally when a selected program
 * reads it, but do not expand the entire stock shader library just for its copy. */
static int sl_shader_stock_include(sh_package_source_link *s, size_t scope,
    const sh_compiled_resource *file)
{
    if (strncmp(file->engine_path, "decls/renderprogs/", 18)) return 0;
    sl_original *original = sl_original_get(s, file->engine_path);
    if (!original || original->status < 0) return -1;
    if (original->status != 1) return 0;
    size_t length = 0;
    unsigned char *body = sh_package_compilation_read(s->scopes[scope], file, SIZE_MAX,
        &length, s->error, s->error_capacity);
    if (!body) { sl_env_fail(s, "shader include could not be read", file->engine_path); return -1; }
    unsigned char *owned = NULL;
    const unsigned char *stock = length == original->resource.body_length ?
        sl_original_bytes(s, original, &owned, NULL, 0) : NULL;
    if (length == original->resource.body_length && !stock) { free(body); return -1; }
    int same = stock && !memcmp(body, stock, length);
    free(owned); free(body); return same;
}
static int sl_seed_inputs(sh_package_source_link *s)
{
    for (size_t scope = 0; scope < s->scope_count; scope++) {
        const sh_package_compilation *original = s->scopes[scope];
        sh_package_compilation filtered = *original; sh_package_link_inputs roots = {0};
        filtered.resources = calloc(original->resource_count ? original->resource_count : 1, sizeof(*filtered.resources));
        filtered.resource_count = 0;
        if (!filtered.resources) return sl_env_fail(s, "cannot enumerate shader source inputs", NULL);
        int ok = 1;
        for (size_t i = 0; ok && i < original->resource_count; i++) {
            const sh_compiled_resource *file = original->resources + i; char *name = NULL;
            if (s->native.audio && sh_package_audio_path_kind(file->engine_path)) continue;
            /* A cooked movie and its atlas are one constructor unit, whichever
             * member this scope supplies; the other resolves in scope then originals. */
            sh_package_movie_names movie;
            if (sh_package_movie_names_for_path(file->engine_path, &movie) == 1) {
                sl_unit *unit = sl_find(s, scope, "file", movie.movie, 1);
                if (!unit || !sl_member(s, unit, file)) { ok = 0; break; }
                continue;
            }
            /* A typed consumer can name this file through another native
             * spelling. Give that exact identity first ownership of the file. */
            if (sl_deferred_binary_family(s, file)) continue;
            int auxiliary = sh_package_virtual_material_auxiliary(original, file, s->error, s->error_capacity);
            if (auxiliary < 0) { ok = 0; break; }
            if (auxiliary) {
                if (!s->native.material_companions) ok = sl_fail(s, "virtual texture source has no material binding adapter", file->engine_path);
                continue;
            }
            int shader = sl_shader_seed(file, &name);
            snprintf(s->seed_note, sizeof(s->seed_note), "scope %zu shader file %s", scope, file->engine_path);
            if (shader < 0) ok = 0;
            else if (name) ok = sl_find(s, scope, "renderprog", name, 1) != NULL;
            else if (!shader) filtered.resources[filtered.resource_count++] = *file;
            else {
                int stock = sl_shader_stock_include(s, scope, file);
                if (stock < 0) { ok = 0; break; }
                if (stock) continue;
                /* A shared shader source reaches the SnapMap programs catalogued
                 * beneath it. Campaign-only programs are not SnapMap resources;
                 * a scope that needs one supplies its declaration, which seeds it. */
                const sh_resource_catalog_entry *const *rows = NULL;
                size_t count = sh_resource_catalog_find_path(s->catalog, file->engine_path, &rows);
                for (size_t r = 0; ok && r < count; r++)
                    if (rows[r]->archive < 2 && !strcmp(rows[r]->type, "renderprog") && *rows[r]->name)
                        ok = sl_find(s, scope, "renderprog", rows[r]->name, 1) != NULL;
            }
            free(name);
        }
        const sh_package_compilation *one = &filtered;
        if (ok) ok = sh_package_link_inputs_open(&one, 1, s->catalog, &roots, s->error, s->error_capacity);
        for (size_t i = 0; ok && i < roots.count; i++) {
            snprintf(s->seed_note, sizeof(s->seed_note), "scope %zu supplied %s", scope,
                roots.units[i].file_count ? roots.units[i].files[0]->engine_path : roots.units[i].name);
            sl_unit *unit = sl_find(s, scope, roots.units[i].family, roots.units[i].name, 1);
            if (!unit) { ok = 0; break; }
            for (size_t f = 0; ok && f < roots.units[i].file_count; f++) {
                const sh_compiled_resource *file = sl_lookup_supplied(s, scope, roots.units[i].files[f]->engine_path);
                ok = file && sl_member(s, unit, file);
            }
        }
        sh_package_link_inputs_free(&roots); free(filtered.resources);
        if (ok && s->native.material_seeds) {
            sl_material_seed walk = {s, scope};
            snprintf(s->seed_note, sizeof(s->seed_note), "scope %zu material companion", scope);
            ok = s->native.material_seeds(s->native.context, scope, sl_seed_material, &walk,
                s->error, s->error_capacity);
        }
        if (ok && s->native.audio) {
            sl_material_seed walk = {s, scope};
            snprintf(s->seed_note, sizeof(s->seed_note), "scope %zu audio program", scope);
            ok = sh_package_audio_program_seed(s->native.audio, scope, s->catalog, sl_seed_sound, &walk,
                s->error, s->error_capacity);
        }
        if (!ok) { s->failed_scope = scope; return 0; }
    }
    return 1;
}
static int sl_seed_orphan_binaries(sh_package_source_link *s)
{
    for (size_t scope = 0; scope < s->scope_count; scope++) {
        const sh_package_compilation *compiled = s->scopes[scope];
        for (size_t f = 0; f < compiled->resource_count; f++) {
            const sh_compiled_resource *file = compiled->resources + f;
            const char *family = sl_deferred_binary_family(s, file);
            if (!family) continue;
            snprintf(s->seed_note, sizeof(s->seed_note), "scope %zu unowned binary %s", scope, file->engine_path);
            int owned = 0;
            for (size_t i = 0; !owned && i < s->count; i++) if (s->units[i]->scope == scope)
                for (size_t j = 0; j < s->units[i]->unit->file_count; j++)
                    if (s->units[i]->unit->files[j] == file) { owned = 1; break; }
            if (owned) continue;
            const char *inverse_family = NULL; char *name = NULL;
            int inverse;
            if (!strcmp(family, "font")) {
                size_t n = sl_font_stem(file->engine_path);
                name = malloc(n + 1);
                if (name) { memcpy(name, file->engine_path + 6, n); name[n] = 0; }
                inverse_family = family; inverse = name ? 1 : -1;
            } else inverse = sh_package_cooked_identity(file->engine_path, &inverse_family, &name);
            if (inverse != 1 ||
                !inverse_family || strcmp(inverse_family, family)) {
                free(name); s->failed_scope = scope;
                return sl_fail(s, "unreferenced binary has no native identity", file->engine_path);
            }
            sl_unit *unit = sl_find(s, scope, family, name, 1); free(name);
            if (!unit || !sl_member(s, unit, file)) { s->failed_scope = scope; return 0; }
        }
    }
    return 1;
}
static int sl_shader_binary_path(void *context, sh_package_shader_path_kind kind,
    const char *original, const char *linked)
{
    (void)linked;
    if (kind == SH_PACKAGE_SHADER_BINARY) snprintf(context, 256, "%s", original);
    return 1;
}
/* The other renderer can generate a different permutation set from the same
 * root (for example, platform extension defines). Its unselected declaration
 * remains delivery data alongside its renderer-specific binary. This only
 * accounts the dormant pair; it never certifies or publishes that program.
 * Selected permutations have already passed complete discovery above. */
static int sl_shader_inactive_source(sh_package_source_link *s, size_t scope,
    const sh_compiled_resource *file, const sh_compiled_resource **pair, uint32_t *verified)
{
    const char *path = file->engine_path;
    size_t n = strlen(path);
    if (!s->native.shader || strncmp(path, "generated/renderprogs/permutations/", 35) ||
        n <= 40 || strcmp(path + n - 5, ".decl")) return 0;
    char binary_path[256] = ""; sh_package_shader_program empty = {0};
    sh_package_shader_renderer other = s->native.shader->renderer == SH_PACKAGE_SHADER_OPENGL ?
        SH_PACKAGE_SHADER_VULKAN : SH_PACKAGE_SHADER_OPENGL;
    if (sh_package_shader_paths(other, path, path, &empty, sl_shader_binary_path, binary_path,
        s->error, s->error_capacity) != SH_PACKAGE_SHADER_OK) return -1;
    const sh_compiled_resource *binary_file = NULL;
    int status = sl_source_file(s, scope, binary_path, 1, &binary_file);
    if (status != 1) return status;
    size_t declaration_length = 0, binary_length = 0;
    unsigned char *declaration = sh_package_compilation_read(s->scopes[scope], file, SIZE_MAX,
        &declaration_length, s->error, s->error_capacity);
    unsigned char *binary = declaration ? sh_package_compilation_read(s->scopes[scope], binary_file, SIZE_MAX,
        &binary_length, s->error, s->error_capacity) : NULL;
    int paired = 0;
    if (!declaration || !binary) { sl_env_fail(s, "inactive shader pair could not be read", path); paired = -1; }
    else if (binary_length >= 8 && !memcmp(binary, "\xb6\x88\x00\xad", 4)) {
        uint32_t checksum = (uint32_t)binary[4] << 24 | (uint32_t)binary[5] << 16 |
            (uint32_t)binary[6] << 8 | binary[7];
        paired = checksum == sh_package_shader_declaration_checksum(declaration, declaration_length);
        if (paired) { *pair = binary_file; *verified = checksum; }
    }
    free(declaration); free(binary); return paired;
}
#ifdef SH_PACKAGE_SOURCE_LINK_TESTING
static long g_sl_unpublished_failures;
void sh_package_source_link_test_unpublished_failures(long count) { g_sl_unpublished_failures = count; }
#endif
/* Diagnostics only: a record that cannot be kept marks the set lost. */
static void sl_unpublished_add(sh_package_source_link *s, size_t scope, const sh_compiled_resource *file,
    sh_package_shader_unpublished_reason reason, const sh_compiled_resource *pair, uint32_t checksum)
{
    if (s->unpublished_lost) return;
    if (s->unpublished_count == s->unpublished_capacity) {
        size_t next = s->unpublished_capacity ? s->unpublished_capacity * 2 : 16;
        sh_package_shader_unpublished *grown = next <= SIZE_MAX / sizeof(*grown) ?
            realloc(s->unpublished, next * sizeof(*grown)) : NULL;
#ifdef SH_PACKAGE_SOURCE_LINK_TESTING
        if (grown && g_sl_unpublished_failures > 0 && g_sl_unpublished_failures--) {
            s->unpublished = grown; grown = NULL;
        }
#endif
        if (!grown) { s->unpublished_lost = 1; return; }
        s->unpublished = grown; s->unpublished_capacity = next;
    }
    s->unpublished[s->unpublished_count++] = (sh_package_shader_unpublished){scope, file, pair, reason,
        s->native.shader ? (int)s->native.shader->renderer : -1, checksum};
}
static int sl_shader_accounted(sh_package_source_link *s)
{
    s->unpublished_count = 0; s->unpublished_lost = 0;
    for (size_t scope = 0; scope < s->scope_count; scope++) for (size_t f = 0; f < s->scopes[scope]->resource_count; f++) {
        const sh_compiled_resource *file = s->scopes[scope]->resources + f;
        const char *path = file->engine_path; char *name = NULL;
        int shader = sl_shader_seed(file, &name); free(name);
        if (shader < 0) return 0;
        if (!shader) continue;
        /* The other renderer and driver cache/debug files remain intact source
         * artifacts; only the chosen renderer has a publication obligation.
         * Each input accepted without a linked owner keeps its reason. */
        size_t n = strlen(path);
        int cache = !strncmp(path, "generated/glsl/", 15) || !strncmp(path, "generated/glslbinaries/", 23);
        int other = (s->native.shader && s->native.shader->renderer == SH_PACKAGE_SHADER_OPENGL &&
             (!strncmp(path, "generated/spirv/", 16) || (n >= 14 && !strcmp(path + n - 14, "_pc_vulkan.bin")))) ||
            (s->native.shader && s->native.shader->renderer == SH_PACKAGE_SHADER_VULKAN && n >= 7 && !strcmp(path + n - 7, "_pc.bin"));
        int found = 0;
        for (size_t u = 0; !found && u < s->count; u++) if (s->units[u]->scope == scope)
            for (size_t m = 0; !found && m < s->units[u]->unit->file_count; m++) found = s->units[u]->unit->files[m] == file;
        if (found) continue;
        if (cache || other) {
            sl_unpublished_add(s, scope, file, cache ? SH_PACKAGE_SHADER_UNPUBLISHED_DRIVER_CACHE :
                SH_PACKAGE_SHADER_UNPUBLISHED_OTHER_RENDERER, NULL, 0);
            continue;
        }
        int stock = sl_shader_stock_include(s, scope, file);
        if (stock < 0) { s->failed_scope = scope; return 0; }
        if (stock) { sl_unpublished_add(s, scope, file, SH_PACKAGE_SHADER_UNPUBLISHED_STOCK_INCLUDE, NULL, 0); continue; }
        const sh_compiled_resource *pair = NULL; uint32_t checksum = 0;
        int dormant = sl_shader_inactive_source(s, scope, file, &pair, &checksum);
        if (dormant < 0) { s->failed_scope = scope; return 0; }
        if (!dormant) { s->failed_scope = scope; return sl_fail(s, "shader source input has no verified companion owner", path); }
        sl_unpublished_add(s, scope, file, SH_PACKAGE_SHADER_UNPUBLISHED_DORMANT_PAIR, pair, checksum);
    }
    return 1;
}
/* file: the captured file being bound, whose sites are recorded; NULL otherwise. */
/* A unit's implicit requests by interned slot (sl_implicit_add): for a walk that adds many to one unit (the late movie
 * requests), so each finds its slot at once and the list grows geometrically. Open addressing over 1 + index. */
typedef struct sl_slot_index { uint32_t *slots; size_t capacity, room; } sl_slot_index;
typedef struct sl_discover {
    sh_package_source_link *source; sl_unit *unit; const sh_compiled_resource *file;
    sl_replay *record; /* discovery replay: collects this walk's requests in order */
    sl_slot_index *index; /* NULL: requests are found by a scan of the unit's list */
} sl_discover;
/* Record one resolved request of the discovery being recorded. */
static void sl_replay_ref_add(sl_replay *r, const char *slot, const char *family, const char *name, int request)
{
    if (!r || !r->valid) return;
    if (r->ref_count == r->ref_capacity) {
        size_t next = r->ref_capacity ? r->ref_capacity * 2 : 16;
        sl_replay_ref *grown = next < SIZE_MAX / sizeof(*grown) ? realloc(r->refs, next * sizeof(*grown)) : NULL;
        if (!grown) { r->valid = 0; return; }
        r->refs = grown; r->ref_capacity = next;
    }
    sl_replay_ref ref = {_strdup(slot), _strdup(family), _strdup(name), request};
    if (!ref.slot || !ref.family || !ref.name) {
        free(ref.slot); free(ref.family); free(ref.name); r->valid = 0; return;
    }
    r->refs[r->ref_count++] = ref;
}
static int sl_player_hands_add(sh_package_source_link *s, const char *name)
{
    for (size_t i = 0; i < s->player_hand_count; i++) if (!strcmp(s->player_hands[i], name)) return 1;
    char *copy = _strdup(name);
    char **grown = copy && s->player_hand_count < SIZE_MAX / sizeof(*grown) - 1 ?
        realloc(s->player_hands, (s->player_hand_count + 1) * sizeof(*grown)) : NULL;
    if (!grown) { free(copy); return 0; }
    s->player_hands = grown; s->player_hands[s->player_hand_count++] = copy; return 1;
}
/* ---- Authored state and the stock route's campaign boundary (R-295) --------
 * A unit's own state is authored when its text is its scope's own compiled
 * text (a package file, built-in or generated result) or a product original
 * (original scope 3), or when authored state inherits it. A declaration
 * only the campaign archive carries is absent from SnapMap's archives. Named
 * by authored state, it is a dependency the authored text needs and lacks:
 * refused, as before. Named only by stock state that no authored state
 * inherits, it is the stock route's own reference: that stock text is served
 * as it is and nothing is supplied under the name, so the engine resolves the
 * name for the served package as it does on the stock route, whatever that
 * yields (sl_files keeps it as an identity with no source). A unit judged
 * that way and then named by authored state is refused when that request
 * arrives. */
static int sl_unit_supplied(const sl_unit *unit);
static int sl_authored(const sh_package_source_link *s, const sl_unit *unit)
{
    if (unit->authored_layer || sl_unit_supplied(unit)) return 1;
    const sh_package_compilation *scope = unit->scope < s->scope_count ? s->scopes[unit->scope] : NULL;
    for (size_t i = 0; i < unit->unit->file_count; i++) {
        const sh_compiled_resource *file = unit->unit->files[i];
        if (SL_EXTRA(unit)->prepared && file == unit->extra->prepared && unit->extra->unprepared) file = unit->extra->unprepared;
        /* The scope's own compiled text (a package file, built-in or generated
         * result) rather than a captured original; product originals too. */
        int own = scope && scope->resources && file >= scope->resources &&
            file < scope->resources + scope->resource_count && !file->restored_original;
        if (own || file->baseline_known == 3 || file->generated_layer) return 1;
    }
    return 0;
}
static int sl_reuse_unmarked(const sh_package_source_link *s, const sl_unit *target);
static int sl_authored_mark(sh_package_source_link *s, sl_unit *target, int layer, unsigned depth)
{
    /* A stock unit a restored scope bound in this pass stands for the copy a fresh pass would discover, mark and
     * then drop at its fold: it takes no mark (R-348). */
    if (sl_reuse_unmarked(s, target)) return 0;
    if (target->campaign_boundary) {
        sl_fail(s, "campaign dependency is absent from its authored scope", target->unit->name); return -1;
    }
    target->authored_request = 1;
    if (!layer || target->authored_layer) return 0;
    target->authored_layer = 1;
    if (!target->discovery->discovered_done) return 0;
    if (depth > 256) { sl_env_fail(s, "authored inheritance is too deep", target->unit->name); return -1; }
    /* Discovered before authored state inherited it: what it names is authored now. */
    for (size_t i = 0; i < target->site_count; i++)
        if (sl_authored_mark(s, target->discovery->sites[i].target, target->discovery->sites[i].slot &&
            !strcmp(target->discovery->sites[i].slot, "inherit"), depth + 1) < 0) return -1;
    for (size_t i = 0; i < target->discovery->dependency_count; i++)
        if (sl_authored_mark(s, target->discovery->dependencies[i], 0, depth + 1) < 0) return -1;
    return 0;
}
static int sl_reference(void *context, const char *slot, const char *family, const char *name, const char **replacement)
{
    sl_discover *walk = context;
    sl_unit *unit = walk->unit;
    sl_unit *target = sl_find(walk->source, unit->scope, family, name, 1);
    *replacement = NULL;
    if (!target) return -1;
    if (sl_authored(walk->source, unit) &&
        sl_authored_mark(walk->source, target, slot && !strcmp(slot, "inherit"), 0) < 0) return -1;
    if (unit->discovery->dependency_count == UINT32_MAX ||
        !sl_arena_grow(&walk->source->dependency_arena, (void **)&unit->discovery->dependencies, &unit->discovery->dependency_capacity,
        unit->discovery->dependency_count, sizeof(*unit->discovery->dependencies))) {
        sl_env_fail(walk->source, "cannot retain scoped dependency", name); return -1;
    }
    unit->discovery->dependencies[unit->discovery->dependency_count++] = target;
    if (walk->file || (walk->source->observe && !strcmp(unit->unit->family,"snapmoduleinfo"))) {
        char *copy = slot && unit->site_count < UINT32_MAX ? (char *)sl_intern(walk->source, slot) : NULL;
        if (copy && !sl_arena_grow(&walk->source->site_arena, (void **)&unit->discovery->sites, &unit->discovery->site_capacity,
            unit->site_count, sizeof(*unit->discovery->sites))) copy = NULL;
        if (!copy) { sl_env_fail(walk->source, "cannot retain a bound reference site", name); return -1; }
        unit->discovery->sites[unit->site_count++] = (sl_site){walk->file, copy, target};
    }
    if (walk->record) sl_replay_ref_add(walk->record, slot, family, name, 0);
    return 0;
}
static size_t sl_slot_index_at(const sl_slot_index *index, const sl_implicit *implicit, const char *slot)
{
    uint64_t h = ((uint64_t)(uintptr_t)slot >> 3) * UINT64_C(11400714819323198485);
    size_t mask = index->capacity - 1, at = (size_t)(h ^ (h >> 29)) & mask;
    while (index->slots[at] && implicit[index->slots[at] - 1].slot != slot) at = (at + 1) & mask;
    return at;
}
/* Index unit's implicit requests for a walk that adds up to more of them (their slots are interned, so equal slots
 * are one pointer). 0 on allocation failure: the walk scans instead. */
static int sl_slot_index_open(sl_slot_index *index, const sl_unit *unit, size_t more)
{
    const sl_discovery *d = unit->discovery;
    size_t need = d->implicit_count + more, capacity = 64;
    memset(index, 0, sizeof(*index));
    while (capacity < need * 2 + 2) { if (capacity > SIZE_MAX / 4 / sizeof(*index->slots)) return 0; capacity *= 2; }
    if (!(index->slots = calloc(capacity, sizeof(*index->slots)))) return 0;
    index->capacity = capacity; index->room = d->implicit_count;
    for (uint32_t i = 0; i < d->implicit_count; i++) {
        size_t at = sl_slot_index_at(index, d->implicit, d->implicit[i].slot);
        if (!index->slots[at]) index->slots[at] = i + 1;
    }
    return 1;
}
static int sl_implicit_add(sl_discover *walk, const char *slot, const char *family, const char *name, int conditional)
{
    sl_unit *unit = walk->unit;
    sl_slot_index *index = walk->index;
    if (!slot || !*slot) return sl_fail(walk->source, "implicit native request has no semantic slot", name);
    sl_unit *target = sl_find(walk->source, unit->scope, family, name, 1);
    if (!target) return 0;
    if (sl_authored(walk->source, unit) && sl_authored_mark(walk->source, target, 0, 0) < 0) return 0;
    /* An indexed walk interns the slot first and finds it by pointer; otherwise the list is scanned by text. */
    const char *interned = index ? sl_intern(walk->source, slot) : NULL;
    if (index && !interned) return sl_env_fail(walk->source, "cannot retain implicit native request", name);
    size_t at = index ? sl_slot_index_at(index, unit->discovery->implicit, interned) : 0;
    size_t found = index ? (index->slots[at] ? index->slots[at] - 1 : SIZE_MAX) : SIZE_MAX;
    for (size_t i = 0; !index && i < unit->discovery->implicit_count && found == SIZE_MAX; i++)
        if (!strcmp(unit->discovery->implicit[i].slot, slot)) found = i;
    if (found != SIZE_MAX) {
        if (unit->discovery->implicit[found].target != target || unit->discovery->implicit[found].conditional != conditional)
            return sl_fail(walk->source, "implicit native slot has contradictory targets", slot);
        if (walk->record) sl_replay_ref_add(walk->record, slot, family, name, 1);
        return 1;
    }
    if (unit->discovery->implicit_count >= UINT32_MAX || unit->discovery->dependency_count == UINT32_MAX ||
        (index && unit->discovery->implicit_count + 1 > index->capacity / 2))
        return sl_env_fail(walk->source, "too many implicit native requests", name);
    char *copy = (char *)(index ? interned : sl_intern(walk->source, slot));
    sl_implicit *grown = unit->discovery->implicit;
    if (!index || unit->discovery->implicit_count == index->room) {
        size_t room = index ? (index->room ? index->room * 2 : 16) : unit->discovery->implicit_count + 1;
        grown = copy && room <= SIZE_MAX / sizeof(*grown) ? realloc(unit->discovery->implicit, room * sizeof(*grown)) : NULL;
        if (grown && index) index->room = room;
    }
    if (!grown) return sl_env_fail(walk->source, "cannot retain implicit native request", name);
    unit->discovery->implicit = grown;
    if (index) index->slots[at] = unit->discovery->implicit_count + 1;
    unit->discovery->implicit[unit->discovery->implicit_count++] = (sl_implicit){copy, target, conditional};
    if (!sl_arena_grow(&walk->source->dependency_arena, (void **)&unit->discovery->dependencies, &unit->discovery->dependency_capacity,
        unit->discovery->dependency_count, sizeof(*unit->discovery->dependencies)))
        return sl_env_fail(walk->source, "cannot retain implicit native dependency", name);
    unit->discovery->dependencies[unit->discovery->dependency_count++] = target;
    if (walk->record) sl_replay_ref_add(walk->record, slot, family, name, 1);
    return 1;
}
static int sl_implicit_reference(void *context, const char *slot, const char *family, const char *name)
{
    return sl_implicit_add(context, slot, family, name, 0);
}
/* A request of the native implicit adapter, which may answer per scope: shared stock re-homes no such unit. */
static int sl_native_request(void *context, const char *slot, const char *family, const char *name)
{
    ((sl_discover *)context)->unit->discovery->native_requests = 1;
    return sl_implicit_add(context, slot, family, name, 0);
}
static int sl_font_material(void *context, const char *slot, const char *material, int optional)
{
    sl_discover *walk = context; sh_package_source_link *s = walk->source;
    char *name = sh_package_engine_path(material);
    char *path = name ? sl_decl_path("material", name) : NULL;
    const sh_compiled_resource *file = NULL;
    int status = path ? sl_source_file(s, walk->unit->scope, path, optional, &file) : -1;
    if (status == 1) {
        sl_unit *target = sl_find(s, walk->unit->scope, "material", name, 1);
        if (!target || !sl_member(s, target, file) || !sl_implicit_reference(walk, slot, "material", name)) status = -1;
    }
    free(path); free(name); return status;
}
static int sl_font_capture(sh_package_source_link *s, sl_unit *unit)
{
    sh_package_font_member members[3] = {0}; size_t count = unit->unit->file_count;
    sl_discover walk = {s, unit}; int ok = 0;
    if (!count && s->observe) return 1;
    if (!count || count > 3) return sl_gap_fail(s, "font has invalid captured glyph forms", unit->unit->name);
    for (size_t i = 0; i < count; i++) {
        members[i].path = unit->unit->files[i]->engine_path;
        members[i].body = sh_package_compilation_read(unit->unit->compilation, unit->unit->files[i],
            SIZE_MAX, &members[i].length, s->error, s->error_capacity);
        if (!members[i].body) goto done;
    }
    /* Exact installed originals keep the engine's own behavior, including a
     * shipped envelope its loader rejects: no request is captured or routed. */
    int native_owned = 1, rejected = 0;
    for (size_t i = 0; i < count; i++) {
        native_owned &= unit->unit->files[i]->native_original != 0;
        rejected |= sh_font_binary_native_rejected(members[i].body, members[i].length);
    }
    if (native_owned && rejected) { ok = 1; goto done; }
    ok = sh_package_font_capture(unit->unit->name, members, count, sl_font_material, &walk, s->error, s->error_capacity);
    if (!ok && !s->failed) s->reader_gap = 1;
done:
    for (size_t i = 0; i < count; i++) free((void *)members[i].body);
    return ok;
}
/* Cooked UI movies. The idFileResource movie and the constructor-derived atlas
 * image form one unit; dictionary fonts, the language resolver's fallback
 * fonts and the fixed GUI materials are constructor requests bound in this
 * scope. All of them are native create-or-default lookups, so a positively
 * absent request is retained as absence rather than borrowed from a peer. */
static int sl_movie_request(void *context, const char *slot, const char *family, const char *name)
{
    sl_discover *walk = context; sh_package_source_link *s = walk->source;
    const sh_compiled_resource *file = NULL;
    sl_unit *target;
    int status;
    if (!strcmp(family, "font")) {
        const sh_compiled_resource *forms[3] = {0}; int present = 0;
        for (unsigned i = 0; i < 3; i++) {
            sh_font_binary_paths paths;
            if (!sh_font_binary_paths_for(name, (sh_font_binary_variant)i, &paths)) {
                sl_env_fail(s, "cannot construct font source paths", name); return -1;
            }
            status = sl_source_file(s, walk->unit->scope, paths.file, 1, forms + i);
            sh_font_binary_paths_clear(&paths);
            if (status < 0) return -1;
            present |= status == 1;
        }
        if (!present) return 0;
        if (!(target = sl_find(s, walk->unit->scope, "font", name, 1))) return -1;
        for (unsigned i = 0; i < 3; i++) if (forms[i] && !sl_member(s, target, forms[i])) return -1;
    } else {
        char *path = sl_decl_path(family, name);
        status = path ? sl_source_file(s, walk->unit->scope, path, 1, &file) : -1;
        free(path);
        if (status != 1) return status;
        if (!(target = sl_find(s, walk->unit->scope, family, name, 1)) || !sl_member(s, target, file)) return -1;
    }
    return sl_implicit_reference(walk, slot, family, name) == 1 ? 1 : -1;
}
static int sl_movie_files(sh_package_source_link *s, sl_unit *unit)
{
    sh_package_movie_names names;
    const sh_compiled_resource *movie = NULL, *atlas = NULL;
    if (sh_package_movie_names_for_path(unit->unit->name, &names) != 1)
        return sl_gap_fail(s, "cooked movie pair cannot be derived from its native name", unit->unit->name);
    int movie_status = sl_source_file(s, unit->scope, names.movie, 0, &movie);
    int atlas_status = movie_status < 0 ? -1 : sl_source_file(s, unit->scope, names.atlas, 0, &atlas);
    if (movie_status < 0 || atlas_status < 0) return 0;
    if (!movie_status || !atlas_status) {
        /* Observation keeps an absent stock pair as a boundary. A delivered
         * half pair cannot construct: the constructor loads both files. */
        if (s->observe && !movie_status && !atlas_status) return 1;
        return sl_gap_fail(s, "cooked movie and its atlas must both be available", unit->unit->name);
    }
    if (!sl_member(s, unit, movie) || !sl_member(s, unit, atlas)) return 0;
    if (unit->discovery->movie_captured) return 1;
    size_t length = 0;
    unsigned char *body = sh_package_compilation_read(unit->unit->compilation, movie, SIZE_MAX,
        &length, s->error, s->error_capacity);
    if (!body) return sl_env_fail(s, "cooked movie could not be read", names.movie);
    sl_discover walk = {s, unit};
    int ok = sh_package_movie_capture(body, length, sl_movie_request, &walk, s->error, s->error_capacity);
    free(body);
    if (!ok) return s->failed ? 0 : sl_gap_fail(s, "cooked movie constructor requests are unsupported", unit->unit->name);
    unit->discovery->movie_captured = 1; return 1;
}
typedef struct sl_movie_sites { sh_package_binding_resolver reference; void *context; size_t record; } sl_movie_sites;
/* Freeze the finite namespace after reverse nomination. These resources and
 * their transitive sources are already captured in this scope; neither the stock catalog nor
 * script literals are a namespace. Certified unchanged targets are conditional
 * edges, so they keep ordinary baseline lookup and do not rename a movie. */
static int sl_movie_late_request(sl_discover *walk, const sl_unit *target)
{
    sh_package_source_link *s = walk->source;
    if (strcmp(target->unit->family, "sound") && strcmp(target->unit->family, "material")) return 1;
    size_t n = strlen(target->unit->family) + strlen(target->unit->name);
    char *slot = n <= SIZE_MAX - 10 ? malloc(n + 10) : NULL;
    if (!slot) return sl_env_fail(s, "cannot retain late movie namespace", walk->unit->unit->name);
    snprintf(slot, n + 10, "request/%s/%s", target->unit->family, target->unit->name);
    int ok = sl_implicit_add(walk, slot, target->unit->family, target->unit->name, 1);
    free(slot); return ok;
}
static int sl_movie_late_requests(sh_package_source_link *s)
{
    if (s->observe) return 1;
    /* Every scope's own sound and material units (not stock only), in unit order, gathered once (R-348: each movie
     * scanned every unit twice); the requests below take them in that order. */
    size_t *first = calloc(s->scope_count + 1, sizeof(*first)), *members = NULL, *cursor = NULL, gathered = 0;
    if (!first) return sl_env_fail(s, "cannot retain late movie namespace", NULL);
    for (size_t j = 0; j < s->count; j++) {
        const sl_unit *target = s->units[j];
        if (target->stock_only || target->scope >= s->scope_count ||
            (strcmp(target->unit->family, "sound") && strcmp(target->unit->family, "material"))) continue;
        first[target->scope + 1]++; gathered++;
    }
    for (size_t k = 0; k < s->scope_count; k++) first[k + 1] += first[k];
    members = malloc((gathered ? gathered : 1) * sizeof(*members));
    cursor = malloc((s->scope_count ? s->scope_count : 1) * sizeof(*cursor));
    if (!members || !cursor) { free(first); free(members); free(cursor); return sl_env_fail(s, "cannot retain late movie namespace", NULL); }
    memcpy(cursor, first, s->scope_count * sizeof(*cursor));
    for (size_t j = 0; j < s->count; j++) {
        const sl_unit *target = s->units[j];
        if (target->stock_only || target->scope >= s->scope_count ||
            (strcmp(target->unit->family, "sound") && strcmp(target->unit->family, "material"))) continue;
        members[cursor[target->scope]++] = j;
    }
    free(cursor);
    int ok = 1;
    const size_t units = s->count; /* the requests resolve existing units only (sl_find), so the lists stay whole */
    for (size_t i = 0; ok && i < s->count; i++) {
        if (s->count != units) { ok = sl_env_fail(s, "late movie requests added units", NULL); break; }
        sl_unit *movie = s->units[i];
        if (!sh_package_movie_owner(movie->unit->family, movie->unit->name)) continue;
        /* Its scope's sound and material units: its own, and (shared stock) the baseline units it binds instead of
         * its own copies; a baseline movie's are the baseline scope's members only. One request each: the movie's
         * requests are indexed for the walk (sl_slot_index), added in the same order with the same checks. */
        const sl_share_map *map = s->share.maps && movie->scope != s->share.baseline ? s->share.maps + movie->scope : NULL;
        size_t from = movie->scope < s->scope_count ? first[movie->scope] : 0;
        size_t to = movie->scope < s->scope_count ? first[movie->scope + 1] : 0, more = to - from;
        for (size_t j = 0; map && j < map->count; j++)
            more += !strcmp(map->units[j]->unit->family, "sound") || !strcmp(map->units[j]->unit->family, "material");
        sl_slot_index index;
        sl_discover walk = {s, movie, NULL, NULL, sl_slot_index_open(&index, movie, more) ? &index : NULL};
        for (size_t j = from; ok && j < to; j++) ok = sl_movie_late_request(&walk, s->units[members[j]]);
        for (size_t j = 0; ok && map && j < map->count; j++) ok = sl_movie_late_request(&walk, map->units[j]);
        if (walk.index) free(index.slots);
    }
    free(first); free(members);
    return ok;
}
static int sl_movie_resolve(void *context, const char *family, const char *name, const char **replacement)
{
    sl_movie_sites *sites = context; char slot[48], normalized[256];
    *replacement = NULL;
    if (!sh_package_movie_native_name(name, strlen(name), normalized, sizeof(normalized))) return -1;
    snprintf(slot, sizeof(slot), "movie/dictionary/%zu", sites->record++);
    return sites->reference(sites->context, slot, family, normalized, replacement);
}
static unsigned char *sl_movie_bind(sl_unit *unit, const sh_compiled_resource *file,
    const unsigned char *body, size_t length, sh_package_binding_resolver reference, void *context,
    size_t *output_length, char *error, size_t capacity)
{
    sh_package_movie_names names;
    if (sh_package_movie_names_for_path(unit->unit->name, &names) != 1 || !file || !file->engine_path) {
        snprintf(error, capacity, "cooked movie unit lost its native pair"); return NULL;
    }
    if (!strcmp(file->engine_path, names.atlas)) {
        /* The atlas is pixel data; its identity is bound by name derivation. */
        unsigned char *out = length == SIZE_MAX ? NULL : malloc(length + 1);
        if (out) { if (length) memcpy(out, body, length); out[length] = 0; *output_length = length; }
        return out;
    }
    if (strcmp(file->engine_path, names.movie)) {
        snprintf(error, capacity, "file is outside its captured movie unit"); return NULL;
    }
    sl_movie_sites sites = {reference, context, 0}; size_t changed = 0;
    return sh_resource_rebind_with(sh_package_movie_material_sites, NULL, SH_BINDING_IDSTR,
        body, length, sl_movie_resolve, &sites, output_length, &changed, error, capacity);
}
/* Paired native identities follow the frozen, path-sorted member order. */
static int sl_movie_identities(sh_package_source_link *s, sl_unit *unit)
{
    sh_package_movie_names names;
    if (!sh_package_movie_owner(unit->unit->family, unit->unit->name) || !unit->unit->file_count) return 1;
    if (unit->unit->file_count != 2 || sh_package_movie_names_for_path(unit->unit->name, &names) != 1)
        return sl_fail(s, "cooked movie unit does not hold exactly its native pair", unit->unit->name);
    sh_package_link_file_identity *identities = calloc(2, sizeof(*identities));
    char *atlas = _strdup(names.atlas);
    if (!identities || !atlas) { free(identities); free(atlas); return sl_env_fail(s, "cannot retain movie identities", unit->unit->name); }
    for (size_t i = 0; i < 2; i++) {
        const char *path = unit->unit->files[i]->engine_path;
        if (!strcmp(path, names.atlas)) { identities[i].family = "image"; identities[i].name = atlas; atlas = NULL; }
        else if (strcmp(path, names.movie)) {
            free((char *)identities[0].name); free(identities); free(atlas);
            return sl_fail(s, "cooked movie unit holds an unrelated file", path);
        }
    }
    if (atlas) { free(identities); free(atlas); return sl_fail(s, "cooked movie unit lost its atlas", unit->unit->name); }
    sl_extra *extra = sl_extra_of(unit);
    if (!extra) {
        for (size_t i = 0; i < 2; i++) free((char *)identities[i].name);
        free(identities); return sl_env_fail(s, "cannot retain cooked movie member identities", unit->unit->name);
    }
    if (extra->identities) for (size_t i = 0; i < 2; i++) free((char *)extra->identities[i].name);
    free(extra->identities);
    extra->identities = identities; unit->unit->file_identities = identities; return 1;
}
static int sl_discover_unit_requests(sh_package_source_link *s, sl_unit *unit);
static int sl_discover_unit(sh_package_source_link *s, sl_unit *unit)
{
    sl_unit *previous = s->discovering;
    s->discovering = unit;
    int ok = sl_discover_unit_requests(s, unit);
    if (ok) unit->discovery->discovered_done = 1;
    s->discovering = previous; return ok;
}
/* ---- Discovery replay ---------------------------------------------------
 * Every package scope rediscovers the stock units its content reaches, most
 * of them unchanged, and a later compile rediscovers every unit again. A
 * reader's discovery of a unit depends only on the unit's files and on the
 * paths it looks up (sh_package_source_reader_lookups: the scope's file,
 * else the one captured original), beyond what the memo key covers. So the
 * reads one discovery made (its native preparation output, reference sites
 * and material requests, in order) are exactly the reads another discovery
 * of the same family and name would make when every one of those files and
 * paths resolves to the same content. Replay then resolves each recorded
 * request in its own scope, as a fresh bind would, and still runs everything
 * scope-dependent itself: file selection, material companions and native
 * constructor requests. Within one capture an unsupplied path is the same
 * captured original everywhere; across captures its identity is compared. */
static void sl_replay_free(sl_replay *r)
{
    if (!r) return;
    for (size_t i = 0; r->files && i < r->file_count; i++) free(r->files[i].path);
    for (size_t i = 0; i < r->lookup_count; i++) free(r->lookups[i].path);
    for (size_t i = 0; i < r->ref_count; i++) { free(r->refs[i].slot); free(r->refs[i].family); free(r->refs[i].name); }
    free(r->lookups); free(r->refs); free(r->file_refs); free(r->files); free(r->prepared);
    free(r->family); free(r->name); free(r);
}
static void sl_lookup(void *context, const char *path, int supplied)
{
    sh_package_source_link *s = context; sl_replay *r = s->recording;
    if (!r || !r->valid) return;
    if (!path) { r->valid = 0; return; }
    for (size_t i = 0; i < r->lookup_count; i++) if (!strcmp(r->lookups[i].path, path)) return;
    if (r->lookup_count == r->lookup_capacity) {
        size_t next = r->lookup_capacity ? r->lookup_capacity * 2 : 8;
        sl_replay_input *grown = next < SIZE_MAX / sizeof(*grown) ? realloc(r->lookups, next * sizeof(*grown)) : NULL;
        if (!grown) { r->valid = 0; return; }
        r->lookups = grown; r->lookup_capacity = next;
    }
    sl_replay_input *input = r->lookups + r->lookup_count;
    memset(input, 0, sizeof(*input));
    if (!(input->path = _strdup(path))) { r->valid = 0; return; }
    input->state = supplied ? -1 : 0; r->lookup_count++;
}
static void sl_store_clear(sl_store *store)
{
    for (size_t i = 0; i < store->capacity; i++)
        for (sl_replay *r = store->slots[i], *next; r; r = next) { next = r->next; sl_replay_free(r); }
    free(store->slots); memset(store, 0, sizeof(*store));
}
static size_t sl_store_slot(sl_replay *const *slots, size_t capacity, const char *family, const char *name)
{
    size_t at = (size_t)sl_hash_text(sl_hash_text(UINT64_C(14695981039346656037), family), name) & (capacity - 1);
    while (slots[at] && (strcmp(slots[at]->family, family) || strcmp(slots[at]->name, name))) at = (at + 1) & (capacity - 1);
    return at;
}
static sl_replay *sl_store_find(const sl_store *store, const sl_unit *unit)
{
    return store && store->capacity ? store->slots[sl_store_slot(store->slots, store->capacity,
        unit->unit->family, unit->unit->name)] : NULL;
}
/* Recordings retained per family and name, newest first; and in all. 1 when r was kept. */
#define SL_REPLAY_VARIANTS 8u
#define SL_REPLAY_RECORDS 262144u
static int sl_store_add(sh_package_source_link *s, sl_replay *r)
{
    sl_store *store = s->store;
    if (store->records >= SL_REPLAY_RECORDS) { sl_replay_free(r); return 0; } /* Replay is optional. */
    if (store->count + 1 > store->capacity / 2) {
        size_t capacity = store->capacity ? store->capacity * 2 : 1024;
        sl_replay **slots = capacity < SIZE_MAX / sizeof(*slots) ? calloc(capacity, sizeof(*slots)) : NULL;
        if (!slots) { sl_replay_free(r); return 0; }
        for (size_t i = 0; i < store->capacity; i++) if (store->slots[i])
            slots[sl_store_slot(slots, capacity, store->slots[i]->family, store->slots[i]->name)] = store->slots[i];
        free(store->slots); store->slots = slots; store->capacity = capacity;
    }
    size_t at = sl_store_slot(store->slots, store->capacity, r->family, r->name), kept = 1;
    if (!store->slots[at]) store->count++;
    r->next = store->slots[at]; store->slots[at] = r; store->records++; s->census.recorded++;
    for (sl_replay *p = r; p->next;) {
        if (++kept <= SL_REPLAY_VARIANTS) { p = p->next; continue; }
        sl_replay *drop = p->next; p->next = drop->next; sl_replay_free(drop); store->records--;
    }
    return 1;
}
/* file when the unit's scope compilation supplies it (one of its resources). */
static const sh_compiled_resource *sl_supplied_file(const sh_package_source_link *s, size_t scope,
    const sh_compiled_resource *file)
{
    const sh_package_compilation *compiled = s->scopes[scope];
    uintptr_t at = (uintptr_t)file, first = (uintptr_t)compiled->resources;
    return compiled->resource_count && at >= first &&
        at < first + compiled->resource_count * sizeof(*compiled->resources) ? file : NULL;
}
static int sl_original_identify(sh_package_source_link *s, sl_original *original, sh_package_file_identity *out)
{
    if (!original->identified) {
        unsigned long long hashing = sl_now_us();
        unsigned char *owned = NULL;
        const unsigned char *bytes = sl_original_bytes(s, original, &owned, NULL, 0);
        if (!bytes) return 0;
        int ok = sh_package_bytes_identity(bytes, original->resource.body_length, &original->identity);
        free(owned);
        if (!ok) return sl_env_fail(s, "cannot identify captured original", original->resource.engine_path);
        original->identified = 1;
        s->census.identify_us += sl_now_us() - hashing;
    }
    *out = original->identity; return 1;
}
/* How path resolves for a reader of scope in this capture: supplied is the
 * scope's own file, else the captured original (captured now if needed, as
 * the reader itself would). 0 when that original cannot be captured. */
static int sl_input_resolve(sh_package_source_link *s, size_t scope, const char *path,
    const sh_compiled_resource *supplied, int identify, sl_replay_input *out)
{
    memset(&out->identity, 0, sizeof(out->identity));
    if (supplied) {
        out->state = -1;
        return !identify || sl_lookup_identity(s, scope, supplied, &out->identity, NULL, 0);
    }
    sl_original *original = sl_original_get(s, path);
    if (!original || original->status < 0) return 0;
    out->state = original->status;
    return !identify || !original->status || sl_original_identify(s, original, &out->identity);
}
/* A recorded file or path resolves the same way now. This capture's own
 * recordings read the same captured originals, so only supplied content is
 * compared; an earlier capture's are compared in full. */
static int sl_input_same(sh_package_source_link *s, size_t scope, const sl_replay_input *input,
    const sh_compiled_resource *supplied, int same_capture)
{
    sl_replay_input now;
    if ((input->state == -1) != (supplied != NULL)) return 0;
    if (same_capture && !supplied) return 1;
    if (!sl_input_resolve(s, scope, input->path, supplied, 1, &now)) return 0;
    return now.state == input->state && now.identity.length == input->identity.length &&
        !memcmp(now.identity.digest, input->identity.digest, sizeof(now.identity.digest));
}
/* A unit whose every file is its scope's own or a captured original. Shader
 * programs and flags (native shader ABI), fonts and cooked movies keep their
 * own capture paths. */
static int sl_replay_eligible(const sh_package_source_link *s, const sl_unit *unit)
{
    if (!g_sl_replay || s->observe || !s->store || !unit->unit->file_count || unit->discovery->preparation_checked ||
        !strcmp(unit->unit->family, "renderprog") || !strcmp(unit->unit->family, "renderprogflag") ||
        !strcmp(unit->unit->family, "font") || sh_package_movie_owner(unit->unit->family, unit->unit->name)) return 0;
    for (size_t i = 0; i < unit->unit->file_count; i++) {
        const sh_compiled_resource *file = unit->unit->files[i];
        const sl_original *original = file ? sl_original_peek(s, file->engine_path) : NULL;
        if (!file || (!sl_supplied_file(s, unit->scope, file) && (!original || &original->resource != file))) return 0;
    }
    return 1;
}
static sl_replay *sl_replay_open(sh_package_source_link *s, const sl_unit *unit)
{
    static volatile __int64 serials;
    sl_replay *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->family = _strdup(unit->unit->family); r->name = _strdup(unit->unit->name);
    r->scope = unit->scope; r->capture = s->capture; r->serial = (uint64_t)_InterlockedIncrement64(&serials);
    r->files = calloc(unit->unit->file_count, sizeof(*r->files));
    r->file_refs = calloc(unit->unit->file_count, sizeof(*r->file_refs));
    if (!r->family || !r->name || !r->files || !r->file_refs) { sl_replay_free(r); return NULL; }
    r->file_count = unit->unit->file_count;
    for (size_t i = 0; i < r->file_count; i++) {
        const sh_compiled_resource *file = unit->unit->files[i];
        if (!(r->files[i].path = _strdup(file->engine_path))) { sl_replay_free(r); return NULL; }
        r->files[i].state = sl_supplied_file(s, unit->scope, file) ? -1 : sl_original_peek(s, file->engine_path)->status;
    }
    r->valid = 1;
    return r;
}
/* Finish a recording: how every consulted path resolved, and each input's
 * content identity when a memo will keep it for later captures. */
static int sl_replay_complete(sh_package_source_link *s, sl_replay *r)
{
    int identify = s->memo != NULL;
    /* Without a memo, recordings serve other scopes of this capture only:
     * one that read supplied content is not kept. */
    for (size_t i = 0; !identify && r->valid && i < r->file_count; i++) if (r->files[i].state == -1) r->valid = 0;
    for (size_t i = 0; !identify && r->valid && i < r->lookup_count; i++) if (r->lookups[i].state == -1) r->valid = 0;
    for (size_t i = 0; r->valid && i < r->file_count; i++) {
        sl_replay_input *input = r->files + i;
        const sh_compiled_resource *supplied = input->state == -1 ? sl_lookup_input(s, r->scope, r, input) : NULL;
        if ((input->state == -1 && !supplied) || !sl_input_resolve(s, r->scope, input->path, supplied, identify, input)) r->valid = 0;
    }
    for (size_t i = 0; r->valid && i < r->lookup_count; i++) {
        sl_replay_input *input = r->lookups + i;
        const sh_compiled_resource *supplied = input->state == -1 ? sl_lookup_input(s, r->scope, r, input) : NULL;
        if ((input->state == -1 && !supplied) || !sl_input_resolve(s, r->scope, input->path, supplied, identify, input)) r->valid = 0;
    }
    return r->valid;
}
/* The same files, and every recorded path resolving to the same content. */
static int sl_replay_applies(sh_package_source_link *s, const sl_unit *unit, const sl_replay *r)
{
    int same_capture = r->capture == s->capture;
    if (!r->valid || r->file_count != unit->unit->file_count || (same_capture && r->scope == unit->scope)) return 0;
    for (size_t i = 0; i < r->file_count; i++) {
        const sh_compiled_resource *file = unit->unit->files[i];
        if (strcmp(file->engine_path, r->files[i].path) ||
            !sl_input_same(s, unit->scope, r->files + i, sl_supplied_file(s, unit->scope, file), same_capture)) return 0;
    }
    for (size_t i = 0; i < r->lookup_count; i++)
        if (!sl_input_same(s, unit->scope, r->lookups + i,
            sl_lookup_input(s, unit->scope, r, r->lookups + i), same_capture)) return 0;
    return 1;
}
/* What a memo's recordings depend on besides the paths they validate: the
 * catalog rows readers consult directly, the native managers and reflection
 * they bind through (read afresh), the product's script capture (immutable,
 * owned by the product) and the preparation callback whose output a
 * recording keeps. */
/* The readers' native digest (sh_package_source_reader_native_digest), read once for the capture: every scope's reader
 * shares the first reader's native metadata (sh_package_source_reader_open_shared), whose loaded answers stay as they
 * were first read, so the memo key and every scope key see the same digest. */
static int sl_native_digest(sh_package_source_link *s, unsigned char out[32])
{
    if (!s->native_digested) {
        if (!s->scope_count || !sh_package_source_reader_native_digest(s->readers[0], s->native_digest)) return 0;
        s->native_digested = 1;
    }
    memcpy(out, s->native_digest, 32); return 1;
}
static int sl_memo_key(sh_package_source_link *s, sh_decl_registry_source registry, unsigned char key[32])
{
    struct { char magic[32]; unsigned char catalog[32], native[32]; uint64_t scripts, prepare; } material;
    sh_package_file_identity identity;
    memset(&material, 0, sizeof(material));
    memcpy(material.magic, "snapmap-plus source memo 1", 26);
    if ((s->catalog && !sh_resource_catalog_digest(s->catalog, material.catalog)) ||
        !sl_native_digest(s, material.native)) return 0;
    material.scripts = (uint64_t)(uintptr_t)registry.scripts;
    material.prepare = (uint64_t)(uintptr_t)s->native.prepare;
    if (!sh_package_bytes_identity(&material, sizeof(material), &identity)) return 0;
    memcpy(key, identity.digest, 32); return 1;
}
sh_package_source_memo *sh_package_source_memo_open(void)
{
    return calloc(1, sizeof(sh_package_source_memo));
}
static void sl_reverse_graph_free(sl_reverse_graph *g);
void sh_package_source_memo_close(sh_package_source_memo *memo)
{
    if (!memo) return;
    sl_store_clear(&memo->store); sl_reverse_graph_free(memo->reverse); free(memo);
}
/* Serve a unit's one file as its prepared text: a copy of that file's metadata that owns only body, allocated
 * for prepared units alone. Takes body; 0 with the failure recorded. */
static int sl_prepared_set(sh_package_source_link *s, sl_unit *unit, unsigned char *body, size_t length)
{
    sl_extra *extra = sl_extra_of(unit);
    if (!extra || (!extra->prepared && (SL_REFUSED(g_sl_prepared_refusal) ||
        !(extra->prepared = malloc(sizeof(*extra->prepared)))))) {
        free(body); return sl_env_fail(s, "cannot retain a prepared source", unit->unit->name);
    }
    extra->unprepared = unit->unit->files[0];
    *extra->prepared = *extra->unprepared;
    extra->prepared->body = body; extra->prepared->body_length = length; extra->prepared->body_owner = NULL;
    ((const sh_compiled_resource **)unit->unit->files)[0] = extra->prepared;
    return 1;
}
static int sl_discovered_add(sh_package_source_link *s, sl_unit *unit, const sh_compiled_resource *file)
{
    const sh_compiled_resource **grown = unit->discovered_count < UINT32_MAX && unit->discovered_count < SIZE_MAX / sizeof(*grown) - 1 ?
        realloc((void *)unit->discovered, (unit->discovered_count + 1) * sizeof(*grown)) : NULL;
    if (!grown) return sl_env_fail(s, "cannot retain a discovered file", unit->unit->name);
    unit->discovered = grown; unit->discovered[unit->discovered_count++] = file; return 1;
}
static int sl_replay_apply(sh_package_source_link *s, sl_unit *unit, const sl_replay *r)
{
    if (s->native.prepare && unit->unit->file_count == 1) {
        if (r->was_prepared) {
            unsigned char *body = malloc(r->prepared_length + 1);
            if (!body) return sl_env_fail(s, "cannot copy a prepared source", unit->unit->name);
            memcpy(body, r->prepared, r->prepared_length); body[r->prepared_length] = 0;
            if (!sl_prepared_set(s, unit, body, r->prepared_length)) return 0;
        }
        unit->discovery->preparation_checked = 1;
    }
    size_t at = 0;
    for (size_t j = 0; j < unit->unit->file_count; j++) {
        sl_discover walk = {s, unit, unit->unit->files[j], NULL};
        for (; at < r->file_refs[j]; at++) {
            const sl_replay_ref *ref = r->refs + at;
            const char *replacement = NULL;
            if (ref->request ? sl_implicit_reference(&walk, ref->slot, ref->family, ref->name) != 1 :
                sl_reference(&walk, ref->slot, ref->family, ref->name, &replacement) != 0) return 0;
        }
        if (!sl_discovered_add(s, unit, walk.file)) return 0;
    }
    if (r->coverage) unit->coverage = 1;
    return !s->failed;
}
/* Prepare, bind and request every file of a unit: the reads of its
 * discovery. record, when set, collects them for replay. */
static int sl_discover_files(sh_package_source_link *s, sl_unit *unit, sl_replay *record)
{
    if (!unit->discovery->preparation_checked && s->native.prepare && unit->unit->file_count == 1) {
        unsigned char *body = NULL; size_t length = 0;
        int status = s->native.prepare(s->native.context, s->readers[unit->scope],
            unit->unit, &body, &length, s->error, s->error_capacity);
        if (status < 0) {
            free(body);
            if (status == -2) s->malformed = s->reader_gap = 1;
            else if (status == -3) s->reader_gap = 1;
            else s->environment = 1;
            return 0;
        }
        if (status == 1 && body) {
            if (record && record->valid) {
                if ((record->prepared = malloc(length + 1))) {
                    memcpy(record->prepared, body, length); record->prepared[length] = 0;
                    record->prepared_length = length; record->was_prepared = 1;
                } else record->valid = 0;
            }
            if (!sl_prepared_set(s, unit, body, length)) return 0;
        } else if (status || body) {
            free(body); return sl_env_fail(s, "invalid prepared source contract", unit->unit->name);
        }
        unit->discovery->preparation_checked = 1;
    }
    for (size_t j = 0; j < unit->unit->file_count; j++) {
        size_t length = 0, bound_length = 0;
        /* Diagnostics: an unsupplied original has no input source files. */
        int original = !unit->unit->files[j]->sources && unit->unit->files[j]->source == SIZE_MAX;
        /* A streamed original is read again through its shared body (metered). */
        int again = !unit->unit->files[j]->body && unit->unit->files[j]->body_owner;
        unsigned long long reading = again ? sl_now_us() : 0;
        unsigned char *body = sh_package_compilation_read(unit->unit->compilation, unit->unit->files[j],
            SIZE_MAX, &length, s->error, s->error_capacity), *bound = NULL;
        if (again) {
            s->share.rereads++; s->share.reread_us += sl_now_us() - reading;
            if (body) s->share.reread_bytes += length;
        }
        /* Linking binds this file with the same reader over the same bytes:
         * keep its sites for the first identity capture (observation none). */
        sl_discover walk = {s, unit, s->observe ? NULL : unit->unit->files[j], record};
        if (body) bound = sl_bind_member(s, unit, unit->unit->files[j],
            body, length, sl_reference, &walk, &bound_length, s->error, s->error_capacity);
        if (body && !bound && !s->failed && sh_package_source_reader_preparing(s->readers[unit->scope])) {
            /* A shader parameter or flag awaits native preparation. */
            free(body); if (record) record->valid = 0;
            return sl_defer_preparation(s);
        }
        int ok = body && bound && length == bound_length && !memcmp(body, bound, length) && !s->failed;
        if (ok && walk.file && !sl_discovered_add(s, unit, walk.file)) { free(body); free(bound); return 0; }
        if (ok && !strcmp(unit->unit->family, "material") &&
            !strncmp(unit->unit->files[j]->engine_path, "generated/decls/material/", 24))
            ok = sh_package_source_material_requests(s->readers[unit->scope],
                (sh_decl_source){(const char *)body, length}, s->observe,
                sl_implicit_reference, &walk, s->error, s->error_capacity);
        if (!ok && body && bound && !s->failed) s->reader_gap = 1;
        if (!ok && body && !bound && !s->failed) s->reader_gap = 1;
        if (!body) s->environment = 1; /* An unreadable captured file blames no input. */
        free(body); free(bound);
        s->census.binds++;
        if (original) s->census.original_binds++;
        if (!ok) return 0;
        if (record) record->file_refs[j] = record->ref_count;
    }
    return 1;
}
/* ---- AI state-machine webs -------------------------------------------------
 * Each AI builds its state-machine web at spawn from every aifsmmanager
 * declaration whose graph layers meet the configuration its behavior names
 * (edit.aiFSMManagerDeclLayersConfig), and reads that configuration from the
 * first declaration, states/alertcycle, whether or not its own graphs are
 * selected (fsm_web.h; Vulkan 0x108BFB0, OpenGL 0x107DB20). A behavior that
 * names a configuration therefore depends on states/alertcycle, and on every
 * declaration the configuration selects in its scope. Both are conditional
 * (sh_package_link_unit): they bind a behavior only to a changed declaration,
 * so an unchanged web leaves its identity as it was. */
static int sl_fsm_catalog(sh_package_source_link *s)
{
    if (s->fsm_names_ready) return 1;
    for (size_t i = 0; s->catalog && i < sh_resource_catalog_count(s->catalog); i++) {
        const sh_resource_catalog_entry *row = sh_resource_catalog_at(s->catalog, i);
        int seen = 0;
        if (row->archive >= 2 || strcmp(row->type, "aifsmmanager") || !*row->name) continue;
        for (size_t k = 0; k < s->fsm_name_count && !seen; k++) seen = !strcmp(s->fsm_names[k], row->name);
        if (seen) continue;
        char **grown = realloc(s->fsm_names, (s->fsm_name_count + 1) * sizeof(*grown));
        if (!grown) return sl_env_fail(s, "cannot list state-machine declarations", row->name);
        s->fsm_names = grown;
        if (!(s->fsm_names[s->fsm_name_count] = _strdup(row->name)))
            return sl_env_fail(s, "cannot list state-machine declarations", row->name);
        s->fsm_name_count++;
    }
    s->fsm_names_ready = 1; return 1;
}
static int sl_fsm_listed(const sh_package_source_link *s, const char *name)
{
    for (size_t i = 0; i < s->fsm_name_count; i++) if (!strcmp(s->fsm_names[i], name)) return 1;
    return 0;
}
/* The declaration source a scope resolves (its own file, else the captured
 * original): 1 with the file and, when text is given, malloc-owned bytes;
 * 0 absent; -1 failed (reported). */
static int sl_fsm_source(sh_package_source_link *s, size_t scope, const char *family, const char *name,
    const sh_compiled_resource **file, char **text, size_t *length)
{
    char *path = sl_decl_path(family, name);
    if (!path) { sl_env_fail(s, "cannot construct declaration path", name); return -1; }
    int status = sl_source_file(s, scope, path, 1, file);
    free(path);
    if (status != 1 || !text) return status;
    *text = (char *)sh_package_compilation_read(s->scopes[scope], *file, SIZE_MAX, length, s->error, s->error_capacity);
    if (!*text) { sl_env_fail(s, "cannot read declaration source", name); return -1; }
    return 1;
}
/* The mask of one state-machine declaration's graphs (config NULL), or of one
 * configuration in it, as its scope resolves it. 1 found, 0 absent (no such
 * declaration or configuration, or no graphs), -1 failed (reported). */
static int sl_fsm_layers(sh_package_source_link *s, size_t scope, const char *name, const char *config, uint32_t *mask)
{
    const sh_compiled_resource *file = NULL;
    char *text = NULL, error[256] = "";
    size_t length = 0;
    *mask = 0;
    int status = sl_fsm_source(s, scope, "aifsmmanager", name, &file, NULL, NULL);
    if (status != 1) return status;
    for (size_t i = 0; i < s->fsm_mask_count; i++) {
        const sl_fsm_mask *known = s->fsm_masks + i;
        if (known->file == file && !known->config == !config && (!config || !strcmp(known->config, config))) {
            *mask = known->mask; return known->status;
        }
    }
    if (sl_fsm_source(s, scope, "aifsmmanager", name, &file, &text, &length) != 1) return -1;
    status = config ? sh_package_fsm_config_mask((sh_decl_source){text, length}, config, mask, error, sizeof(error)) :
        sh_package_fsm_graph_mask((sh_decl_source){text, length}, mask, error, sizeof(error));
    free(text);
    if (status < 0) {
        char reason[400];
        snprintf(reason, sizeof(reason), "state-machine %s cannot be read (%s)", config ? "layer configuration" : "graphs", error);
        sl_gap_fail(s, reason, name); return -1;
    }
    if (s->fsm_mask_count == s->fsm_mask_capacity) {
        size_t next = s->fsm_mask_capacity ? s->fsm_mask_capacity * 2 : 64;
        sl_fsm_mask *grown = next < SIZE_MAX / sizeof(*grown) ? realloc(s->fsm_masks, next * sizeof(*grown)) : NULL;
        if (!grown) { sl_env_fail(s, "cannot retain state-machine layers", name); return -1; }
        s->fsm_masks = grown; s->fsm_mask_capacity = next;
    }
    sl_fsm_mask known = {file, config ? _strdup(config) : NULL, *mask, status};
    if (config && !known.config) { sl_env_fail(s, "cannot retain state-machine layers", name); return -1; }
    s->fsm_masks[s->fsm_mask_count++] = known;
    return status;
}
typedef struct sl_fsm_parents { sh_package_source_link *source; size_t scope; } sl_fsm_parents;
static int sl_fsm_parent(void *context, const char *name, sh_decl_source *out)
{
    sl_fsm_parents *parents = context;
    const sh_compiled_resource *file = NULL;
    char *text = NULL; size_t length = 0;
    int status = sl_fsm_source(parents->source, parents->scope, "aibehavior", name, &file, &text, &length);
    if (status == 1) { out->text = text; out->length = length; }
    return status;
}
typedef struct sl_fsm_walk { sh_package_source_link *source; sl_discover *discover; } sl_fsm_walk;
static int sl_fsm_exists(void *context, const char *declaration)
{
    sl_fsm_walk *walk = context;
    const sh_compiled_resource *file = NULL;
    return sl_fsm_source(walk->source, walk->discover->unit->scope, "aifsmmanager", declaration, &file, NULL, NULL);
}
static int sl_fsm_mask_of(void *context, const char *declaration, const char *config, uint32_t *mask)
{
    sl_fsm_walk *walk = context;
    return sl_fsm_layers(walk->source, walk->discover->unit->scope, declaration, config, mask);
}
static int sl_fsm_visit(void *context, const char *name)
{
    sl_fsm_walk *walk = context;
    char slot[600];
    snprintf(slot, sizeof(slot), "fsm:%s", name);
    return sl_implicit_add(walk->discover, slot, "aifsmmanager", name, 1) == 1;
}
static int sl_fsm_edges(sh_package_source_link *s, sl_unit *unit)
{
    const sh_package_compilation *scope = s->scopes[unit->scope];
    sl_fsm_parents parents = {s, unit->scope};
    sh_decl_entity_class_source source = {&parents, sl_fsm_parent, NULL};
    sl_discover discover = {s, unit};
    sl_fsm_walk walk = {s, &discover};
    sh_package_fsm_layers layers = {&walk, sl_fsm_exists, sl_fsm_mask_of};
    const char **names = NULL;
    char *config = NULL, error[512] = "";
    size_t length = 0, count = 0;
    int status, ok;
    if (!g_sl_fsm || strcmp(unit->unit->family, "aibehavior") || !unit->unit->file_count) return 1;
    unsigned char *body = sh_package_compilation_read(unit->unit->compilation, unit->unit->files[0], SIZE_MAX, &length,
        s->error, s->error_capacity);
    if (!body) return sl_env_fail(s, "cannot read behavior declaration", unit->unit->name);
    status = sh_package_fsm_behavior_config((sh_decl_source){(char *)body, length}, &source, &config, error, sizeof(error));
    free(body);
    if (s->failed) { free(config); return 0; }
    if (status < 0) {
        char reason[640];
        snprintf(reason, sizeof(reason), "behavior layer configuration cannot be resolved (%s)", error);
        return sl_gap_fail(s, reason, unit->unit->name);
    }
    if (!status) return 1;
    /* Candidates: the catalog's declarations, then the ones this scope adds. */
    if (!sl_fsm_catalog(s) || !(names = calloc(s->fsm_name_count + scope->resource_count + 1, sizeof(*names)))) {
        free(config); return s->failed ? 0 : sl_env_fail(s, "cannot list state-machine declarations", unit->unit->name);
    }
    for (size_t i = 0; i < s->fsm_name_count; i++) names[count++] = s->fsm_names[i];
    for (size_t i = 0; i < scope->resource_count; i++) {
        const sh_compiled_resource *file = scope->resources + i;
        if (file->type && !strcmp(file->type, "aifsmmanager") && file->name && *file->name && !sl_fsm_listed(s, file->name))
            names[count++] = file->name;
    }
    ok = sh_package_fsm_select(config, &layers, names, count, sl_fsm_visit, &walk);
    free((void *)names);
    sl_extra *extra = ok ? sl_extra_of(unit) : NULL;
    if (ok && !extra) ok = sl_env_fail(s, "cannot retain an AI layer configuration", unit->unit->name);
    if (ok) { free(extra->fsm_config); extra->fsm_config = config; } else free(config);
    return ok && !s->failed;
}
/* ---- Module families ---------------------------------------------------------
 * A SnapMap module's name-derived members (sh_package_module_member) are its
 * implicit dependencies: the engine opens each at a path it derives from the
 * module's own path, which no reference spells. Each member that exists in the
 * module's scope (its file there, the installed original, or the scopes'
 * producer's claim over a product module's base module) is one request
 * "module/<role>/<path>". A changed member therefore changes its module, whose
 * variant publishes every member under its own path (the library's derived
 * publication); a member reached only so is carried (sl_changed_bindings). */
typedef struct sl_module_row { const char *module; size_t row; } sl_module_row;
static int sl_module_row_order(const void *a, const void *b)
{
    const sl_module_row *x = a, *y = b;
    int order = strcmp(x->module, y->module);
    return order ? order : x->row < y->row ? -1 : x->row > y->row;
}
/* Every installed member row by its module, built once per capture. */
static int sl_module_rows(sh_package_source_link *s)
{
    char module[1024];
    const char *role;
    if (s->module_rows_ready) return 1;
    for (size_t i = 0, n = s->catalog ? sh_resource_catalog_count(s->catalog) : 0; i < n; i++) {
        const sh_resource_catalog_entry *row = sh_resource_catalog_at(s->catalog, i);
        if (!row->path || !*row->path || !row->type || !*row->type || !row->name || !*row->name ||
            row->archive >= 2 || !sh_package_module_member(row->path, module, sizeof(module), &role)) continue;
        if (s->module_row_count == s->module_row_capacity) {
            size_t next = s->module_row_capacity ? s->module_row_capacity * 2 : 1024;
            sl_module_row *grown = next <= SIZE_MAX / sizeof(*grown) ? realloc(s->module_rows, next * sizeof(*grown)) : NULL;
            if (!grown) return 0;
            s->module_rows = grown; s->module_row_capacity = next;
        }
        char *copy = _strdup(module);
        if (!copy) return 0;
        s->module_rows[s->module_row_count++] = (sl_module_row){copy, i};
    }
    if (s->module_row_count > 1) qsort(s->module_rows, s->module_row_count, sizeof(*s->module_rows), sl_module_row_order);
    s->module_rows_ready = 1; return 1;
}
static size_t sl_module_rows_find(const sh_package_source_link *s, const char *module, size_t *first)
{
    size_t low = 0, high = s->module_row_count, count = 0;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if (strcmp(s->module_rows[mid].module, module) < 0) low = mid + 1; else high = mid;
    }
    *first = low;
    while (low + count < s->module_row_count && !strcmp(s->module_rows[low + count].module, module)) count++;
    return count;
}
/* The native identity of a member at path, as the installed catalog names such
 * rows: a card by its declaration name, navigation by its manager name, every
 * other member by its own path. A row the catalog lists keeps its own. */
static int sl_member_identity(const sh_package_source_link *s, const sh_compiled_resource *file,
    const char *path, const char *role, const char **family, char **name)
{
    const sh_resource_catalog_entry *const *rows = NULL;
    size_t length = strlen(path);
    *family = NULL; *name = NULL;
    if (file && file->type && *file->type && file->name && *file->name) {
        *family = file->type; return (*name = _strdup(file->name)) != NULL;
    }
    size_t count = s->catalog ? sh_resource_catalog_find_provider(s->catalog, path, &rows) : 0;
    for (size_t i = 0; i < count; i++) if (rows[i]->type && *rows[i]->type && rows[i]->name && *rows[i]->name) {
        *family = rows[i]->type; return (*name = _strdup(rows[i]->name)) != NULL;
    }
    if (!strcmp(role, "card")) {
        if (length <= 31 + 5) return 0;
        if (!(*name = malloc(length - 31 - 5 + 1))) return 0;
        memcpy(*name, path + 31, length - 31 - 5); (*name)[length - 31 - 5] = 0;
        *family = "snapmoduleinfo"; return 1;
    }
    if (!strcmp(role, "navigation")) {
        const char *kind = NULL;
        if (sh_package_cooked_identity(path, &kind, name) != 1 || !kind || strcmp(kind, "aas")) {
            free(*name); *name = NULL; return 0;
        }
        *family = kind; return 1;
    }
    *family = !strcmp(role, "collision") ? "cm" : length > 7 && !strcmp(path + length - 7, ".bmodel") ? "model" : "file";
    return (*name = _strdup(path)) != NULL;
}
/* One member request of walk's module. verify: path must still be shown to
 * exist in the module's scope (a supplied file, an original or a claim);
 * installed rows exist without reading their bytes. A supplied or verified
 * file is the member's own, whatever catalog spelling would select. */
static int sl_module_edge(sl_discover *walk, const char *path, const char *role, const char *family,
    const char *name, const sh_compiled_resource *file, int verify)
{
    sh_package_source_link *s = walk->source;
    char slot[1100];
    if (snprintf(slot, sizeof(slot), "module/%s/%s", role, path) >= (int)sizeof(slot))
        return sl_fail(s, "module member path exceeds its native slot", path);
    if (verify && !file) {
        int status = sl_source_file(s, walk->unit->scope, path, 1, &file);
        if (status < 0) return 0;
        if (status != 1) return 1; /* absent from this module's scope */
    }
    /* A producer may serve the member under its own canonical name: the unit
     * is the one that file's identity names, the slot keeps the engine path. */
    char *canonical = NULL;
    if (file && file->engine_path && strcmp(file->engine_path, path)) {
        if (!sl_member_identity(s, file, file->engine_path, role, &family, &canonical))
            return sl_env_fail(s, "cannot name a canonical module member", file->engine_path);
        name = canonical;
    }
    if (file) {
        sl_unit *target = sl_find(s, walk->unit->scope, family, name, 1);
        if (!target || (!target->unit->file_count && !sl_member(s, target, file))) { free(canonical); return 0; }
    }
    int ok = sl_implicit_add(walk, slot, family, name, 0);
    free(canonical); return ok;
}
/* The base module a product module's declaration was generated from: its one
 * generated input that is itself an installed module declaration. */
static int sl_module_base(const sh_package_source_link *s, const sh_compiled_resource *decl, char *base, size_t capacity)
{
    int found = 0;
    for (size_t i = 0; decl->generated && i < decl->generated_input_count; i++) {
        const char *input = decl->generated_inputs[i];
        const sh_resource_catalog_entry *const *rows = NULL;
        size_t length = strlen(input), count = s->catalog ? sh_resource_catalog_find_provider(s->catalog, input, &rows) : 0;
        int module = 0;
        for (size_t r = 0; r < count; r++) module |= rows[r]->type && !strcmp(rows[r]->type, "snapmodule");
        if (!module || length <= 13 + 5 || length - 5 >= capacity) continue;
        if (found) return -1;
        memcpy(base, input, length - 5); base[length - 5] = 0; found = 1;
    }
    return found;
}
static int sl_module_members(sh_package_source_link *s, sl_unit *unit)
{
    char module[1024], member[1024], base[1024];
    const char *role, *family;
    sl_discover walk = {s, unit};
    size_t n = strlen(unit->unit->name), first = 0, count;
    if (n <= 13 + 5 || strncmp(unit->unit->name, "maps/modules/", 13) ||
        strcmp(unit->unit->name + n - 5, ".decl") || n - 5 >= sizeof(module)) return 1;
    memcpy(module, unit->unit->name, n - 5); module[n - 5] = 0;
    if (!sl_module_rows(s)) return sl_env_fail(s, "cannot index installed module members", unit->unit->name);
    count = sl_module_rows_find(s, module, &first);
    for (size_t i = first; i < first + count; i++) {
        const sh_resource_catalog_entry *row = sh_resource_catalog_at(s->catalog, s->module_rows[i].row);
        if (!sh_package_module_member(row->path, NULL, 0, &role) ||
            !sl_module_edge(&walk, row->path, role, row->type, row->name, NULL, !s->observe)) return 0;
    }
    /* Members its scope supplies, including ones the installed module lacks. */
    const sh_package_compilation *compiled = s->scopes[unit->scope];
    for (size_t r = 0; r < compiled->resource_count; r++) {
        const sh_compiled_resource *file = compiled->resources + r;
        char *name = NULL;
        if (!file->engine_path || !sh_package_module_member(file->engine_path, member, sizeof(member), &role) ||
            strcmp(member, module)) continue;
        if (!sl_member_identity(s, file, file->engine_path, role, &family, &name))
            return sl_env_fail(s, "cannot name a supplied module member", file->engine_path);
        int ok = sl_module_edge(&walk, file->engine_path, role, family, name, file, 1);
        free(name);
        if (!ok) return 0;
    }
    /* A product module's members are the producer's claims over its base
     * module's members, each under the product module's path. */
    if (unit->unit->file_count == 1 && unit->unit->files[0]->generated) {
        int based = sl_module_base(s, unit->unit->files[0], base, sizeof(base));
        if (based < 0) return sl_fail(s, "product module declaration reads several base modules", unit->unit->name);
        count = based ? sl_module_rows_find(s, base, &first) : 0;
        for (size_t i = first; i < first + count; i++) {
            const sh_resource_catalog_entry *row = sh_resource_catalog_at(s->catalog, s->module_rows[i].row);
            char *path = sh_package_module_member_rename(row->path, base, module), *name = NULL;
            int ok = path && sh_package_module_member(path, NULL, 0, &role) &&
                sl_member_identity(s, NULL, path, role, &family, &name) &&
                sl_module_edge(&walk, path, role, family, name, NULL, 1);
            if (!path || !name) { free(path); free(name); return sl_env_fail(s, "cannot name a product module member", row->path); }
            free(path); free(name);
            if (!ok) return 0;
        }
    }
    return 1;
}
/* A supplied member names its module, which then enters the capture in that
 * scope whenever its declaration exists there (sl_module_members carries the
 * change). Called after seeding, for the seeds only. */
static int sl_seed_modules(sh_package_source_link *s, size_t seeds)
{
    char module[1024], decl[1032];
    const char *role;
    for (size_t i = 0; i < seeds && i < s->count; i++) {
        sl_unit *unit = s->units[i];
        for (size_t f = 0; f < unit->unit->file_count; f++) {
            const sh_compiled_resource *file = NULL;
            const char *path = unit->unit->files[f]->engine_path;
            /* A product's output may carry its producer's canonical name; the
             * producer names the module whose family generates it. */
            if (!path || (!sh_package_module_member(path, module, sizeof(module), &role) &&
                (!s->native.produce_module ||
                 s->native.produce_module(s->native.producer_context, path, module, sizeof(module)) != 1))) continue;
            snprintf(decl, sizeof(decl), "%s.decl", module);
            s->product_optional = 1;
            int status = sl_source_file(s, unit->scope, decl, 1, &file);
            s->product_optional = 0;
            if (status < 0) return 0;
            if (status != 1) continue;
            snprintf(s->seed_note, sizeof(s->seed_note), "module of supplied member %s", path);
            /* A product module has no catalog row: its declaration is the file
             * just resolved (a claim, or the scope's own product). */
            sl_unit *owner = sl_find(s, unit->scope, "snapmodule", decl, 1);
            if (!owner || (!owner->unit->file_count && !sl_member(s, owner, file))) return 0;
        }
    }
    s->seed_note[0] = 0;
    return 1;
}

/* The input rule of shared stock (sl_fold_local): every file and scoped path the unit's discovery read, as its
 * recording (or the one it replayed) lists them, resolved to a captured original in its scope, and the baseline
 * scope supplies none of them, so a discovery of the unit there reads the same. */
static int sl_share_inputs(sh_package_source_link *s, const sl_replay *r)
{
    size_t baseline = s->share.baseline;
    for (size_t i = 0; i < r->file_count; i++)
        if (r->files[i].state < 0 || sl_lookup_input(s, baseline, r, r->files + i)) return 0;
    for (size_t i = 0; i < r->lookup_count; i++)
        if (r->lookups[i].state < 0 || sl_lookup_input(s, baseline, r, r->lookups + i)) return 0;
    return 1;
}
static int sl_share_scope(const sh_package_source_link *s, size_t scope)
{ return s->share.mode && s->share.fold && scope < s->scope_count && s->share.fold[scope]; }
/* Streamed binary originals (R-344 stage A): the discoveries that replay a recorded unit do not read its files, so
 * after its first recorded discovery the capture lets the bytes of its backed (binary) originals go, keeping each
 * one's identity, and tells the snapshot (original_released). A later reader reads them again exactly through the
 * shared body, or fails as an environment failure (sl_original_bytes). Declarations are not backed and stay. */
static int sl_stream_release(sh_package_source_link *s, const sl_unit *unit)
{
    for (size_t i = 0; i < unit->unit->file_count; i++) {
        const sh_compiled_resource *file = unit->unit->files[i];
        sl_original *original = sl_original_peek(s, file->engine_path);
        sh_package_file_identity identity;
        if (!original || &original->resource != file || !original->resource.body || !original->resource.body_owner ||
            !sh_package_body_backed(original->resource.body_owner)) continue;
        if (!sl_original_identify(s, original, &identity)) return 0;
        original->resource.body = NULL; sh_package_body_detach(original->resource.body_owner);
        s->share.released++; s->share.released_bytes += original->resource.body_length;
        if (s->native.original_released)
            s->native.original_released(s->native.original_released_context, file->engine_path);
    }
    return 1;
}
static int sl_discover_unit_requests(sh_package_source_link *s, sl_unit *unit)
{
    unsigned long long began = sl_now_us(), selected, read;
    int streamed = 0;
    if (!sl_files(s, unit)) {
        if (s->observe && !s->failed && !s->preparing) s->reader_gap = 1;
        return 0;
    }
    selected = sl_now_us(); s->census.select_us += selected - began;
    sl_replay *record = NULL;
    if (sl_replay_eligible(s, unit)) {
        int known = 0;
        for (const sl_replay *r = sl_store_find(s->store, unit); r; r = r->next) {
            if (!r->valid) continue;
            known = 1; s->census.replay_tries++;
            unsigned long long captured = s->census.original_us;
            unsigned long long checking = sl_now_us();
            int applies = sl_replay_applies(s, unit, r);
            unsigned long long applying = sl_now_us();
            s->census.applies_us += applying - checking;
            unsigned long long capture_us = s->census.original_us - captured;
            if (applying >= checking && applying - checking > capture_us)
                s->census.applies_without_capture_us += applying - checking - capture_us;
            if (!applies) {
                if (s->failed) return 0; /* A read that failed while validating. */
                continue;
            }
            int applied = sl_replay_apply(s, unit, r);
            s->census.apply_us += sl_now_us() - applying;
            if (!applied) return 0;
            unit->replayed = 1; s->census.replayed++;
            if (r->capture != s->capture) s->census.retained++;
            if (sl_share_scope(s, unit->scope)) unit->discovery->stock_inputs = sl_share_inputs(s, r);
            if (s->reuse.in && !(unit->discovery->consulted = sl_reuse_set_of(s, r))) return 0;
            streamed = 1;
            goto constructed;
        }
        if (known) s->census.declined++;
        record = sl_replay_open(s, unit); /* NULL: optional, bind without recording */
    }
    s->recording = record;
    unsigned long long fresh = sl_now_us();
    int ok = sl_discover_files(s, unit, record);
    s->census.fresh_us += sl_now_us() - fresh;
    s->recording = NULL;
    if (record && ok && record->valid && sl_replay_complete(s, record)) {
        record->coverage = unit->coverage; /* only its own bindings set it (sl_bind_member) */
        if (sl_share_scope(s, unit->scope)) unit->discovery->stock_inputs = sl_share_inputs(s, record);
        /* Before the store may free it: what a restored scope's binding of this unit checks (sl_reuse_judge). */
        if (s->reuse.in && !(unit->discovery->consulted = sl_reuse_set_of(s, record))) { sl_replay_free(record); return 0; }
        streamed = sl_store_add(s, record);
    } else sl_replay_free(record);
    if (s->failed) return 0;
    if (!ok) return 0;
constructed:
    read = sl_now_us(); s->census.read_us += read - selected;
    if (!strcmp(unit->unit->family, "font") && !sl_font_capture(s, unit)) return 0;
    sl_discover walk = {s, unit};
    if (s->native.implicit && s->native.implicit(s->native.context, unit->scope, s->observe,
        unit->unit, sl_native_request, &walk, s->error, s->error_capacity) != 1)
        return sl_fail(s, "native constructor dependencies could not be captured", unit->unit->name);
    if (!strcmp(unit->unit->family,"snapmodule") && !sl_module_members(s,unit)) return 0;
    if (!sl_fsm_edges(s, unit)) return 0;
    s->census.construct_us += sl_now_us() - read;
    if (!s->failed && streamed && g_sl_stream && !sl_stream_release(s, unit)) return 0;
    return !s->failed;
}
/* These are the proven terminal families of the source reader. Reverse
 * reachability needs their identities, not a second copy of large leaf bytes.
 * Ordinary selected-unit linking still verifies their physical source. */
static int sl_reverse_leaf(const sl_unit *unit)
{
    return sh_package_source_leaf(unit->unit->family, unit->unit->name);
}
/* Account one reverse-discovery coverage gap. scope is SIZE_MAX for the stock
 * unit whose reader stopped, or the discovery scope whose changed dependency
 * reaches an incompletely read stock root. Owned copies survive the walk. */
static int sl_reverse_gap_add(sh_package_source_link *s, size_t scope, const sl_unit *unit, const char *reason, int variant)
{
    for (size_t i = 0; i < s->reverse_gap_count; i++) {
        const sh_package_reverse_gap *gap = s->reverse_gaps + i;
        if (gap->scope == scope && !strcmp(gap->family, unit->unit->family) && !strcmp(gap->name, unit->unit->name)) return 1;
    }
    if (s->reverse_gap_count == s->reverse_gap_capacity) {
        size_t next = s->reverse_gap_capacity ? s->reverse_gap_capacity * 2 : 16;
        sh_package_reverse_gap *grown = next > s->reverse_gap_capacity && next <= SIZE_MAX / sizeof(*grown) ?
            realloc(s->reverse_gaps, next * sizeof(*grown)) : NULL;
        if (!grown) return sl_env_fail(s, "cannot account reverse discovery coverage", unit->unit->name);
        s->reverse_gaps = grown; s->reverse_gap_capacity = next;
    }
    sh_package_reverse_gap gap = {scope, _strdup(unit->unit->family), _strdup(unit->unit->name),
        _strdup(reason && *reason ? reason : "reader coverage is incomplete"), variant};
    if (!gap.family || !gap.name || !gap.reason) {
        free((char *)gap.family); free((char *)gap.name); free((char *)gap.reason);
        return sl_env_fail(s, "cannot account reverse discovery coverage", unit->unit->name);
    }
    s->reverse_gaps[s->reverse_gap_count++] = gap; return 1;
}
/* First incompletely read unit reachable from root, for attribution only. */
static const sl_unit *sl_reverse_gap_reached(sh_package_source_link *base, size_t root, unsigned char *seen, size_t *stack)
{
    const sl_unit *found = NULL; size_t depth = 0;
    memset(seen, 0, base->count);
    stack[depth++] = root; seen[root] = 1;
    while (depth && !found) {
        const sl_unit *unit = base->units[stack[--depth]];
        if (unit->discovery->reverse_gap) { found = unit; break; }
        for (size_t d = 0; d < unit->discovery->dependency_count; d++) {
            size_t next = unit->discovery->dependencies[d]->index;
            if (!seen[next]) { seen[next] = 1; stack[depth++] = next; }
        }
    }
    return found;
}
typedef struct sl_reverse_native { sh_package_source_link *source; size_t baseline; } sl_reverse_native;
static int sl_reverse_admit(void *context, size_t scope, const char *family, const char *name,
    unsigned char semantics[32], char *error, size_t capacity)
{
    sl_reverse_native *walk = context;
    (void)scope;
    return walk->source->native.admit ? walk->source->native.admit(walk->source->native.context,
        walk->baseline, family, name, semantics, error, capacity) : 0;
}
static int sl_reverse_implicit(void *context, size_t scope, int observing, const sh_package_link_unit *unit,
    int (*visit)(void *, const char *, const char *, const char *), void *visit_context,
    char *error, size_t capacity)
{
    sl_reverse_native *walk = context; (void)scope;
    return !walk->source->native.implicit || walk->source->native.implicit(walk->source->native.context,
        walk->baseline, observing, unit, visit, visit_context, error, capacity) == 1;
}
static int sl_reverse_prepare(void *context, sh_package_source_reader *reader,
    const sh_package_link_unit *unit, unsigned char **body, size_t *length, char *error, size_t capacity)
{
    sl_reverse_native *walk = context;
    return walk->source->native.prepare(walk->source->native.context, reader, unit, body, length, error, capacity);
}
static int sl_reverse_file_changed(sh_package_source_link *s, size_t scope, const char *path)
{
    const sh_compiled_resource *file = path && *path ? sh_package_compilation_find(s->scopes[scope], path) : NULL;
    return file && sh_package_owners_count(&file->gameplay_owners) != 0;
}
static int sl_reverse_changed(sh_package_source_link *s, size_t scope, const sl_unit *unit)
{
    if (!strcmp(unit->unit->family, "sound") &&
        sh_package_audio_program_affected(s->native.audio, scope, unit->unit->name)) return 1;
    /* What this scope supplies under that identity: a seeded unit as seeded.
     * Seed discovery runs first (sl_discover_through) and may already have
     * added stock dependencies and material companions; neither is supplied. */
    const sl_unit *supplied = sl_find(s, scope, unit->unit->family, unit->unit->name, 0);
    if (supplied && !supplied->discovery->seeded) supplied = NULL;
    for (size_t i = 0; supplied && i < supplied->unit->file_count; i++)
        if (sh_package_owners_count(&supplied->unit->files[i]->gameplay_owners)) return 1;
    for (size_t i = 0; supplied && i < supplied->discovery->seeded_witnesses; i++)
        if (sh_package_owners_count(&supplied->unit->witnesses[i].resource->gameplay_owners)) return 1;
    /* Catalog aliases need not spell the same logical name as a typed field.
     * The inspected unit's physical members still select exactly its source. */
    for (size_t i = 0; i < unit->unit->file_count; i++)
        if (sl_reverse_file_changed(s, scope, unit->unit->files[i]->engine_path)) return 1;
    for (size_t i = 0; i < unit->unit->witness_count; i++)
        if (sl_reverse_file_changed(s, scope, unit->unit->witnesses[i].resource->engine_path)) return 1;
    const sh_resource_catalog_entry *const *rows = NULL;
    size_t count = sh_resource_catalog_find(s->catalog, unit->unit->family, unit->unit->name, &rows);
    for (size_t i = 0; i < count; i++) if (sl_reverse_file_changed(s, scope, rows[i]->path)) return 1;
    /* Terminal families did not need their large payloads read for reverse
     * reachability. Use only the existing verified native path adapter. */
    char *path = sh_package_cooked_path(unit->unit->family, unit->unit->name);
    int changed = sl_reverse_file_changed(s, scope, path); free(path); return changed;
}
/* ---- The player hands web --------------------------------------------------
 * idPlayer::handsAnimWeb (and its _D1 alternate) names the one animation web
 * the hands animator resolves every weapon's subweb in by name (Vulkan
 * FUN_1403c0e30 / FUN_1403c16f0: subweb lookup in the animator's single web).
 * The engine spawns the co-op rules entity mp/modes/coop/game_challenge and
 * the player from its playerSpawnDef, defaulting to player/mp
 * (CreateDefaultChallengeEntityFromSession, Vulkan FUN_14030cb20; the same
 * constant is at OpenGL 0x142017bc0); SnapMap plays the co-op mode. So that
 * web is one global registry of weapon subwebs, whether or not any placed
 * object reaches the player. The stock rules and player decide it. */
typedef struct sl_stock_reader { sh_package_baseline_reader read; void *context; } sl_stock_reader;
static int sl_stock_parent(void *context, const char *name, sh_decl_source *source)
{
    sl_stock_reader *reader = context;
    unsigned char *body = NULL; size_t length = 0;
    char *path = sl_decl_path("entitydef", name);
    int status = path ? reader->read(reader->context, path, &body, &length) : -1;
    free(path);
    if (status <= 0) { free(body); return status; }
    source->text = (const char *)body; source->length = length; return 1;
}
/* One field of a stock entity's expanded edit state; NULL when absent. */
static char *sl_stock_field(sl_stock_reader *reader, const char *entity, const char *field, int *failed)
{
    sh_decl_source source = {0};
    sh_decl_entity_class_source parents = {reader, sl_stock_parent, NULL};
    const char *value = NULL; size_t length = 0;
    char *out = NULL;
    int status = sl_stock_parent(reader, entity, &source);
    if (status < 0) *failed = 1;
    if (status <= 0) return NULL;
    /* Repeated members are native-valid; expansion applies the merge. */
    sh_decl_node *tree = sh_decl_tree_parse_ordered(source, NULL, 0);
    free((char *)source.text);
    sh_decl_node *state = tree ? sh_decl_entity_expanded_state(tree, &parents, NULL, 0) : NULL;
    const sh_decl_node *edit = state ? sh_decl_tree_member(state, "edit") : NULL;
    if (edit && sh_decl_tree_literal(sh_decl_tree_member(edit, field), &value, &length) && length) {
        if ((out = malloc(length + 1))) { memcpy(out, value, length); out[length] = 0; }
        else *failed = 1;
    }
    sh_decl_tree_free(state); sh_decl_tree_free(tree); return out;
}
int sh_package_player_hands_webs(sh_package_baseline_reader original, void *context,
    char ***names, size_t *count)
{
    static const char *const fields[] = {"handsAnimWeb", "handsAnimWeb_D1"};
    sl_stock_reader reader = {original, context};
    int failed = 0;
    *names = NULL; *count = 0;
    if (!original) return 1;
    char *player = sl_stock_field(&reader, "mp/modes/coop/game_challenge", "playerSpawnDef", &failed);
    for (size_t i = 0; !failed && i < sizeof(fields) / sizeof(fields[0]); i++) {
        char *web = sl_stock_field(&reader, player ? player : "player/mp", fields[i], &failed);
        int seen = 0;
        for (size_t k = 0; web && k < *count; k++) seen |= !strcmp((*names)[k], web);
        if (web && !seen) {
            char **grown = realloc(*names, (*count + 1) * sizeof(*grown));
            if (!grown) { free(web); failed = 1; break; }
            *names = grown; (*names)[(*count)++] = web; web = NULL;
        }
        free(web);
    }
    free(player);
    if (!failed) return 1;
    for (size_t k = 0; k < *count; k++) free((*names)[k]);
    free(*names); *names = NULL; *count = 0; return 0;
}
/* The library-wide hands decision a closure compile takes from the whole library (R-397): -1 lets the
 * compile decide from its own scopes; 0 or 1 is the decision the whole compile of the same final state
 * makes, which a compile of only some of its packages cannot see. */
static volatile long g_sl_player_contract_pin = -1;
void sh_package_source_link_pin_player_contract(int supplied) { _InterlockedExchange(&g_sl_player_contract_pin, supplied); }
static int sl_player_contract(sh_package_source_link *s)
{
    int supplied = g_sl_player_contract_pin > 0;
    /* Only a supplied animation web can change the hands web. */
    for (size_t scope = 0; g_sl_player_contract_pin < 0 && scope < s->scope_count && !supplied; scope++)
        for (size_t i = 0; i < s->scopes[scope]->resource_count && !supplied; i++) {
            const sh_compiled_resource *file = s->scopes[scope]->resources + i;
            supplied = file->type && !strcmp(file->type, "animweb") && file->source_count;
        }
    if (!s->original || !supplied) return 1;
    char **names = NULL; size_t count = 0;
    int failed = !sh_package_player_hands_webs(sl_read_original, s, &names, &count);
    for (size_t i = 0; i < count; i++) {
        if (!failed && !sl_player_hands_add(s, names[i])) failed = 1;
        free(names[i]);
    }
    free(names);
    return failed ? sl_env_fail(s, "cannot read the stock player hands web", "mp/modes/coop/game_challenge") : 1;
}
/* Which base units a discovered scope changes, exactly as sl_reverse_changed
 * decides, from what the scope itself supplies: gameplay-owned files, found
 * through the key a lookup of each base unit's candidate path computes (one
 * table per canonicalizer); seeded units with gameplay owners, by identity;
 * affected audio events, for sound units only. Player hands webs never change.
 * Built once per capture: checking every base unit's paths again for every
 * scope repeated the same lookups scope after scope. */
typedef char *(*sl_canonicalizer)(const char *path);
typedef struct sl_reverse_key { char *key; size_t unit; } sl_reverse_key;
#define SL_REVERSE_CANONICALIZERS 8u
typedef struct sl_reverse_index {
    sl_canonicalizer canonical[SL_REVERSE_CANONICALIZERS];
    sl_reverse_key *keys[SL_REVERSE_CANONICALIZERS];
    size_t key_count[SL_REVERSE_CANONICALIZERS], key_capacity[SL_REVERSE_CANONICALIZERS], sets;
    size_t *sounds, sound_count, *hands, hand_count;
    size_t **seeded, *seeded_count; /* per scope: base units its seeded supplies change */
} sl_reverse_index;
static sl_canonicalizer sl_canonicalizer_of(const sh_package_compilation *scope)
{ return scope->canonical_path ? scope->canonical_path : sh_package_engine_path; }
static int sl_reverse_key_order(const void *left, const void *right)
{ return strcmp(((const sl_reverse_key *)left)->key, ((const sl_reverse_key *)right)->key); }
static int sl_reverse_key_add(sl_reverse_index *index, size_t set, sl_canonicalizer canonical, const char *path, size_t unit)
{
    if (!path || !*path) return 1;
    char *key = canonical(path);
    if (!key) return 1; /* no lookup can find an unusable path */
    if (index->key_count[set] == index->key_capacity[set]) {
        size_t next = index->key_capacity[set] ? index->key_capacity[set] * 2 : 1024;
        sl_reverse_key *grown = next <= SIZE_MAX / sizeof(*grown) ? realloc(index->keys[set], next * sizeof(*grown)) : NULL;
        if (!grown) { free(key); return 0; }
        index->keys[set] = grown; index->key_capacity[set] = next;
    }
    index->keys[set][index->key_count[set]++] = (sl_reverse_key){key, unit};
    return 1;
}
static size_t sl_reverse_set(const sl_reverse_index *index, sl_canonicalizer canonical)
{
    for (size_t i = 0; i < index->sets; i++) if (index->canonical[i] == canonical) return i;
    return SIZE_MAX;
}
static void sl_reverse_index_close(sl_reverse_index *index, size_t scopes)
{
    for (size_t set = 0; set < index->sets; set++) {
        for (size_t i = 0; i < index->key_count[set]; i++) free(index->keys[set][i].key);
        free(index->keys[set]);
    }
    for (size_t i = 0; index->seeded && i < scopes; i++) free(index->seeded[i]);
    free(index->seeded); free(index->seeded_count); free(index->sounds); free(index->hands);
}
static int sl_reverse_index_open(sh_package_source_link *s, sh_package_source_link *base,
    const sh_package_source_root_plan *plan, sl_reverse_index *index)
{
    memset(index, 0, sizeof(*index));
    for (size_t scope = 0; scope < s->scope_count; scope++) if (plan->discover[scope]) {
        sl_canonicalizer canonical = sl_canonicalizer_of(s->scopes[scope]);
        if (sl_reverse_set(index, canonical) == SIZE_MAX && index->sets < SL_REVERSE_CANONICALIZERS)
            index->canonical[index->sets++] = canonical;
    }
    index->sounds = malloc((base->count ? base->count : 1) * sizeof(*index->sounds));
    index->hands = malloc((base->count ? base->count : 1) * sizeof(*index->hands));
    index->seeded = calloc(s->scope_count ? s->scope_count : 1, sizeof(*index->seeded));
    index->seeded_count = calloc(s->scope_count ? s->scope_count : 1, sizeof(*index->seeded_count));
    if (!index->sounds || !index->hands || !index->seeded || !index->seeded_count) return 0;
    for (size_t i = 0; i < base->count; i++) {
        const sl_unit *unit = base->units[i];
        if (!strcmp(unit->unit->family, "animweb") && sh_package_source_link_player_hands(s, unit->unit->name))
            index->hands[index->hand_count++] = i;
        if (!strcmp(unit->unit->family, "sound")) index->sounds[index->sound_count++] = i;
        const sh_resource_catalog_entry *const *rows = NULL;
        size_t count = sh_resource_catalog_find(s->catalog, unit->unit->family, unit->unit->name, &rows);
        char *cooked = sh_package_cooked_path(unit->unit->family, unit->unit->name);
        int ok = 1;
        for (size_t set = 0; ok && set < index->sets; set++) {
            for (size_t f = 0; ok && f < unit->unit->file_count; f++)
                ok = sl_reverse_key_add(index, set, index->canonical[set], unit->unit->files[f]->engine_path, i);
            for (size_t w = 0; ok && w < unit->unit->witness_count; w++)
                ok = sl_reverse_key_add(index, set, index->canonical[set], unit->unit->witnesses[w].resource->engine_path, i);
            for (size_t r = 0; ok && r < count; r++) ok = sl_reverse_key_add(index, set, index->canonical[set], rows[r]->path, i);
            if (ok) ok = sl_reverse_key_add(index, set, index->canonical[set], cooked, i);
        }
        free(cooked);
        if (!ok) return 0;
    }
    for (size_t set = 0; set < index->sets; set++)
        if (index->key_count[set] > 1) qsort(index->keys[set], index->key_count[set], sizeof(*index->keys[set]), sl_reverse_key_order);
    /* Seeded supplies with gameplay owners, as sl_reverse_changed reads them. */
    for (size_t i = 0; i < s->count; i++) {
        const sl_unit *supplied = s->units[i];
        int owned = 0;
        if (!supplied->discovery->seeded || supplied->scope >= s->scope_count || !plan->discover[supplied->scope]) continue;
        for (size_t f = 0; !owned && f < supplied->unit->file_count; f++)
            owned = sh_package_owners_count(&supplied->unit->files[f]->gameplay_owners) != 0;
        for (size_t w = 0; !owned && w < supplied->discovery->seeded_witnesses; w++)
            owned = sh_package_owners_count(&supplied->unit->witnesses[w].resource->gameplay_owners) != 0;
        const sl_unit *changed = owned ? sl_find(base, 0, supplied->unit->family, supplied->unit->name, 0) : NULL;
        if (!changed) continue;
        size_t scope = supplied->scope, n = index->seeded_count[scope];
        size_t *grown = n < SIZE_MAX / sizeof(*grown) - 1 ? realloc(index->seeded[scope], (n + 1) * sizeof(*grown)) : NULL;
        if (!grown) return 0;
        index->seeded[scope] = grown; grown[n] = changed->index; index->seeded_count[scope] = n + 1;
    }
    return 1;
}
#ifdef SH_PACKAGE_SOURCE_LINK_TESTING
static size_t g_sl_reverse_checked, g_sl_reverse_mismatches;
void sh_package_source_link_test_reverse(size_t *checked, size_t *mismatches)
{ *checked = g_sl_reverse_checked; *mismatches = g_sl_reverse_mismatches; }
#endif
/* local[i] = 0 for each base unit scope changes; returns how many. */
static size_t sl_reverse_scope_changes(sh_package_source_link *s, sh_package_source_link *base,
    const sl_reverse_index *index, size_t scope, unsigned char *local)
{
    size_t set = sl_reverse_set(index, sl_canonicalizer_of(s->scopes[scope])), changed = 0;
    memset(local, 1, base->count);
    if (set == SIZE_MAX) {
        for (size_t i = 0; i < base->count; i++) {
            const sl_unit *unit = base->units[i];
            if (!strcmp(unit->unit->family, "animweb") && sh_package_source_link_player_hands(s, unit->unit->name)) continue;
            if (sl_reverse_changed(s, scope, unit)) local[i] = 0;
        }
    } else {
        const sh_package_compilation *compiled = s->scopes[scope];
        const sl_reverse_key *keys = index->keys[set];
        size_t count = index->key_count[set];
        for (size_t r = 0; r < compiled->resource_count; r++) {
            const sh_compiled_resource *file = compiled->resources + r;
            if (!file->engine_path || !sh_package_owners_count(&file->gameplay_owners)) continue;
            size_t low = 0, high = count;
            while (low < high) {
                size_t mid = low + (high - low) / 2;
                if (strcmp(keys[mid].key, file->engine_path) < 0) low = mid + 1; else high = mid;
            }
            for (; low < count && !strcmp(keys[low].key, file->engine_path); low++) local[keys[low].unit] = 0;
        }
        for (size_t i = 0; i < index->seeded_count[scope]; i++) local[index->seeded[scope][i]] = 0;
        for (size_t i = 0; i < index->sound_count; i++)
            if (sh_package_audio_program_affected(s->native.audio, scope, base->units[index->sounds[i]]->unit->name))
                local[index->sounds[i]] = 0;
        for (size_t i = 0; i < index->hand_count; i++) local[index->hands[i]] = 1;
    }
#ifdef SH_PACKAGE_SOURCE_LINK_TESTING
    /* Tests: the index decides exactly what checking every base unit decides. */
    for (size_t i = 0; i < base->count; i++) {
        const sl_unit *unit = base->units[i];
        int hands = !strcmp(unit->unit->family, "animweb") && sh_package_source_link_player_hands(s, unit->unit->name);
        int unchanged = hands || !sl_reverse_changed(s, scope, unit);
        g_sl_reverse_checked++;
        if (unchanged != (local[i] != 0)) g_sl_reverse_mismatches++;
    }
#endif
    for (size_t i = 0; i < base->count; i++) changed += !local[i];
    return changed;
}
static const sl_unit *sl_edge(const sl_unit *unit, size_t e, const char **slot);
static int sl_presentation_field(const char *slot);
/* Whether every change a scope makes below a stock card leaves it through its
 * blueprint models, as sl_presentation_slot requires. Only such a card is
 * nominated; one also reached through another field needs a module variant. */
static int sl_card_presentation_reach(const sl_unit *unit, const unsigned char *unchanged)
{
    size_t edges = unit->discovery->dependency_count + unit->site_count + unit->discovery->implicit_count;
    int presented = 0;
    for (size_t e = unit->discovery->dependency_count; e < edges; e++) {
        const char *slot; const sl_unit *target = sl_edge(unit, e, &slot);
        if (!target || target == unit || unchanged[target->index]) continue;
        if (!sl_presentation_field(slot)) return 0;
        presented = 1;
    }
    for (size_t d = 0; d < unit->discovery->dependency_count; d++) {
        const sl_unit *target = unit->discovery->dependencies[d];
        int seen = 0;
        if (!target || target == unit || unchanged[target->index]) continue;
        for (size_t e = unit->discovery->dependency_count; !seen && e < edges; e++) seen = sl_edge(unit, e, NULL) == target;
        if (!seen) return 0;
    }
    return presented;
}
/* Whether a scope's changes reach a module other than through a card change
 * that stays its presentation (sl_card_presentation_reach): its own
 * declaration (local is the scope's direct changes) or any other dependency.
 * Only then does the module need a variant. */
static int sl_module_variant_reach(const sl_unit *unit, const unsigned char *local, const unsigned char *unchanged)
{
    if (!local[unit->index]) return 1;
    for (size_t d = 0; d < unit->discovery->dependency_count; d++) {
        const sl_unit *target = unit->discovery->dependencies[d];
        if (!target || target == unit || unchanged[target->index]) continue;
        if (!strcmp(target->unit->family, "snapmoduleinfo") && sl_card_presentation_reach(target, unchanged)) continue;
        return 1;
    }
    return 0;
}
/* Whether an installed module carries the card of this name. */
static int sl_card_module(const sh_package_source_link *s, const char *card)
{
    const sh_resource_catalog_entry *const *rows = NULL;
    size_t length = strlen(card), count;
    char *decl = length < SIZE_MAX - 20 ? malloc(length + 19) : NULL;
    if (!decl) return 0;
    snprintf(decl, length + 19, "maps/modules/%s.decl", card);
    count = s->catalog ? sh_resource_catalog_find(s->catalog, "snapmodule", decl, &rows) : 0;
    free(decl);
    for (size_t i = 0; i < count; i++) if (rows[i]->archive < 2) return 1;
    return 0;
}
/* ---- Retained reverse graph (R-386) ---------------------------------------
 * The stock reverse base sl_reverse_roots discovers (one scope over the original-only baseline and the catalog)
 * reads what the catalog, the stock originals, the native readers and the renderer's shader adapter say, never
 * what the package scopes supply: its shape is the same for every capture of one catalog, native and renderer
 * identity in one product. The nomination reads only that shape (units by family and name, their files' and
 * witnesses' engine paths, dependency, site and implicit edges with their slots, and the reader gaps accounted
 * while the base was read), so a capture whose caller asserts production stock (reverse_retain) keeps the shape
 * in its memo and the next capture with an equal key rebuilds the base from it. On the dev library the fresh
 * discovery reads 84060 stock units in 14-16 s with about 2.3 GB of transient bytes; the rebuild takes well
 * under a second and about 20 MB. */
typedef struct sl_rg_intern { const char **keys; uint32_t *offsets; size_t capacity, count; } sl_rg_intern;
static uint64_t sl_rg_hash(const char *text)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) hash = (hash ^ *p) * UINT64_C(1099511628211);
    return hash;
}
static void sl_reverse_graph_free(sl_reverse_graph *g)
{
    if (!g) return;
    free(g->units); free(g->targets); free(g->paths); free(g->edges); free(g->gaps); free(g->text); free(g);
}
/* text's offset in g->text, interned; UINT32_MAX for NULL. 0 when memory runs out or the block would overflow. */
static int sl_rg_text(sl_reverse_graph *g, sl_rg_intern *in, size_t *text_capacity, const char *text, uint32_t *out)
{
    if (!text) { *out = UINT32_MAX; return 1; }
    if (!*text) { *out = 0; return 1; }
    if (in->count + 1 > in->capacity / 2) {
        size_t next = in->capacity ? in->capacity * 2 : 4096;
        const char **keys = calloc(next, sizeof(*keys));
        uint32_t *offsets = calloc(next, sizeof(*offsets));
        if (!keys || !offsets) { free(keys); free(offsets); return 0; }
        for (size_t i = 0; i < in->capacity; i++) {
            if (!in->keys[i]) continue;
            size_t at = (size_t)sl_rg_hash(in->keys[i]) & (next - 1);
            while (keys[at]) at = (at + 1) & (next - 1);
            keys[at] = in->keys[i]; offsets[at] = in->offsets[i];
        }
        free(in->keys); free(in->offsets); in->keys = keys; in->offsets = offsets; in->capacity = next;
    }
    size_t at = (size_t)sl_rg_hash(text) & (in->capacity - 1);
    while (in->keys[at]) {
        if (!strcmp(in->keys[at], text)) { *out = in->offsets[at]; return 1; }
        at = (at + 1) & (in->capacity - 1);
    }
    size_t length = strlen(text) + 1;
    if (length > UINT32_MAX - g->text_length) return 0;
    if (g->text_length + length > *text_capacity) {
        size_t next = *text_capacity ? *text_capacity : 65536;
        while (next < g->text_length + length) next *= 2;
        char *grown = realloc(g->text, next);
        if (!grown) return 0;
        /* The interned keys point into the block; re-point them. */
        for (size_t i = 0; i < in->capacity; i++) if (in->keys[i]) in->keys[i] = grown + (in->keys[i] - g->text);
        g->text = grown; *text_capacity = next;
    }
    memcpy(g->text + g->text_length, text, length);
    in->keys[at] = g->text + g->text_length; in->offsets[at] = (uint32_t)g->text_length; in->count++;
    *out = (uint32_t)g->text_length; g->text_length += (uint32_t)length;
    return 1;
}
static int sl_rg_grow(void **array, uint32_t count, uint32_t *capacity, size_t stride)
{
    if (count < *capacity) return 1;
    uint32_t next = *capacity ? (*capacity > UINT32_MAX / 2 ? UINT32_MAX : *capacity * 2) : 1024;
    if (next <= count || next > SIZE_MAX / stride) return 0;
    void *grown = realloc(*array, (size_t)next * stride);
    if (!grown) return 0;
    *array = grown; *capacity = next; return 1;
}
/* The shape of base (sl_reverse_roots' discovered stock reverse base, roots roots) under key, or NULL. */
static sl_reverse_graph *sl_reverse_capture(const sh_package_source_link *s, const sh_package_source_link *base,
    size_t roots, const unsigned char key[32])
{
    sl_reverse_graph *g = calloc(1, sizeof(*g));
    sl_rg_intern in = {0};
    size_t text_capacity = 0;
    uint32_t target_capacity = 0, path_capacity = 0, edge_capacity = 0, gap_capacity = 0;
    int ok = 0;
    if (!g || base->count > UINT32_MAX - 1 || roots > base->count) goto done;
    g->text = malloc(65536); if (!g->text) goto done;
    text_capacity = 65536; g->text[0] = 0; g->text_length = 1;
    g->units = calloc(base->count ? base->count : 1, sizeof(*g->units)); if (!g->units) goto done;
    g->unit_count = (uint32_t)base->count; g->root_count = (uint32_t)roots;
    for (size_t i = 0; i < base->count; i++) {
        const sl_unit *u = base->units[i];
        sl_rg_unit *r = g->units + i;
        if (u->index != i || u->unit->file_count > UINT32_MAX || u->unit->witness_count > UINT32_MAX) goto done;
        if (!sl_rg_text(g, &in, &text_capacity, u->unit->family, &r->family) ||
            !sl_rg_text(g, &in, &text_capacity, u->unit->name, &r->name)) goto done;
        r->files = g->path_count; r->file_count = (uint32_t)u->unit->file_count;
        for (size_t f = 0; f < u->unit->file_count; f++) {
            if (!sl_rg_grow((void **)&g->paths, g->path_count, &path_capacity, sizeof(*g->paths)) ||
                !sl_rg_text(g, &in, &text_capacity, u->unit->files[f] ? u->unit->files[f]->engine_path : NULL,
                    g->paths + g->path_count)) goto done;
            g->path_count++;
        }
        r->witnesses = g->path_count; r->witness_count = (uint32_t)u->unit->witness_count;
        for (size_t w = 0; w < u->unit->witness_count; w++) {
            const sh_compiled_resource *resource = u->unit->witnesses[w].resource;
            if (!sl_rg_grow((void **)&g->paths, g->path_count, &path_capacity, sizeof(*g->paths)) ||
                !sl_rg_text(g, &in, &text_capacity, resource ? resource->engine_path : NULL, g->paths + g->path_count)) goto done;
            g->path_count++;
        }
        r->deps = g->target_count; r->dep_count = u->discovery->dependency_count;
        for (size_t d = 0; d < u->discovery->dependency_count; d++) {
            const sl_unit *target = u->discovery->dependencies[d];
            if (!target || target->index >= base->count || base->units[target->index] != target ||
                !sl_rg_grow((void **)&g->targets, g->target_count, &target_capacity, sizeof(*g->targets))) goto done;
            g->targets[g->target_count++] = target->index;
        }
        r->sites = g->edge_count; r->site_count = u->site_count;
        for (size_t j = 0; j < u->site_count; j++) {
            const sl_site *site = u->discovery->sites + j;
            if (!site->target || site->target->index >= base->count || base->units[site->target->index] != site->target ||
                !sl_rg_grow((void **)&g->edges, g->edge_count, &edge_capacity, sizeof(*g->edges)) ||
                !sl_rg_text(g, &in, &text_capacity, site->slot, &g->edges[g->edge_count].slot)) goto done;
            g->edges[g->edge_count++].target = site->target->index;
        }
        r->implicits = g->edge_count; r->implicit_count = u->discovery->implicit_count;
        for (size_t j = 0; j < u->discovery->implicit_count; j++) {
            const sl_implicit *implicit = u->discovery->implicit + j;
            if (!implicit->target || implicit->target->index >= base->count || base->units[implicit->target->index] != implicit->target ||
                !sl_rg_grow((void **)&g->edges, g->edge_count, &edge_capacity, sizeof(*g->edges)) ||
                !sl_rg_text(g, &in, &text_capacity, implicit->slot, &g->edges[g->edge_count].slot)) goto done;
            g->edges[g->edge_count++].target = implicit->target->index;
        }
        if (u->discovery->reverse_gap) {
            size_t at = (size_t)u->discovery->reverse_gap - 1;
            if (at >= s->reverse_gap_count || s->reverse_gaps[at].scope != SIZE_MAX ||
                strcmp(s->reverse_gaps[at].family, u->unit->family) || strcmp(s->reverse_gaps[at].name, u->unit->name) ||
                !sl_rg_grow((void **)&g->gaps, g->gap_count, &gap_capacity, sizeof(*g->gaps))) goto done;
            sl_rg_gap *gap = g->gaps + g->gap_count;
            gap->family = r->family; gap->name = r->name;
            if (!sl_rg_text(g, &in, &text_capacity, s->reverse_gaps[at].reason, &gap->reason)) goto done;
            r->gap = ++g->gap_count;
        }
    }
    memcpy(g->key, key, 32);
    ok = 1;
done:
    free(in.keys); free(in.offsets);
    if (!ok) { sl_reverse_graph_free(g); g = NULL; }
    return g;
}
/* The blocks of a stock reverse base rebuilt from a retained graph. */
typedef struct sl_rebuilt {
    sl_unit *units; sl_discovery *discoveries; sh_compiled_resource *resources;
    const sh_compiled_resource **files; sh_package_link_witness *witnesses;
    sl_unit **targets; sl_site *sites; sl_implicit *implicits;
} sl_rebuilt;
static void sl_rebuilt_free(sh_package_source_link *base)
{
    sl_rebuilt *r = base->rebuilt;
    free(r->units); free(r->discoveries); free(r->resources); free((void *)r->files); free(r->witnesses);
    free(r->targets); free(r->sites); free(r->implicits); free(r);
    free(base->units); free(base);
}
/* Whether g is internally consistent: every index in range, every text offset inside the block. */
static int sl_reverse_graph_valid(const sl_reverse_graph *g)
{
    if (!g->text_length || g->text[0] || g->text[g->text_length - 1] || g->root_count > g->unit_count) return 0;
#define SL_RG_TEXT_OK(offset) ((offset) == UINT32_MAX || (offset) < g->text_length)
    for (uint32_t i = 0; i < g->unit_count; i++) {
        const sl_rg_unit *r = g->units + i;
        if (!SL_RG_TEXT_OK(r->family) || !SL_RG_TEXT_OK(r->name) || r->family == UINT32_MAX || r->name == UINT32_MAX) return 0;
        if (r->files > g->path_count || r->file_count > g->path_count - r->files) return 0;
        if (r->witnesses > g->path_count || r->witness_count > g->path_count - r->witnesses) return 0;
        if (r->deps > g->target_count || r->dep_count > g->target_count - r->deps) return 0;
        if (r->sites > g->edge_count || r->site_count > g->edge_count - r->sites) return 0;
        if (r->implicits > g->edge_count || r->implicit_count > g->edge_count - r->implicits) return 0;
        if (r->gap > g->gap_count) return 0;
    }
    for (uint32_t i = 0; i < g->path_count; i++) if (!SL_RG_TEXT_OK(g->paths[i])) return 0;
    for (uint32_t i = 0; i < g->target_count; i++) if (g->targets[i] >= g->unit_count) return 0;
    for (uint32_t i = 0; i < g->edge_count; i++) if (!SL_RG_TEXT_OK(g->edges[i].slot) || g->edges[i].target >= g->unit_count) return 0;
    for (uint32_t i = 0; i < g->gap_count; i++)
        if (!SL_RG_TEXT_OK(g->gaps[i].family) || !SL_RG_TEXT_OK(g->gaps[i].name) || !SL_RG_TEXT_OK(g->gaps[i].reason) ||
            g->gaps[i].reason == UINT32_MAX) return 0;
#undef SL_RG_TEXT_OK
    return 1;
}
/* A stock reverse base with g's shape: the units sl_reverse_roots' nomination reads, nothing a discovery needs. */
static sh_package_source_link *sl_reverse_rebuild(const sl_reverse_graph *g)
{
    sh_package_source_link *base = calloc(1, sizeof(*base));
    sl_rebuilt *r = calloc(1, sizeof(*r));
    size_t n = g->unit_count ? g->unit_count : 1, paths = g->path_count ? g->path_count : 1;
    size_t targets = g->target_count ? g->target_count : 1, edges = g->edge_count ? g->edge_count : 1;
    if (!base || !r) { free(base); free(r); return NULL; }
    base->rebuilt = r; base->observe = 1; base->count = base->capacity = g->unit_count;
    base->units = calloc(n, sizeof(*base->units));
    r->units = calloc(n, sizeof(*r->units)); r->discoveries = calloc(n, sizeof(*r->discoveries));
    r->resources = calloc(paths, sizeof(*r->resources)); r->files = calloc(paths, sizeof(*r->files));
    r->witnesses = calloc(paths, sizeof(*r->witnesses)); r->targets = calloc(targets, sizeof(*r->targets));
    r->sites = calloc(edges, sizeof(*r->sites)); r->implicits = calloc(edges, sizeof(*r->implicits));
    if (!base->units || !r->units || !r->discoveries || !r->resources || !r->files || !r->witnesses || !r->targets ||
        !r->sites || !r->implicits) { sl_rebuilt_free(base); return NULL; }
    for (uint32_t p = 0; p < g->path_count; p++)
        r->resources[p].engine_path = g->paths[p] == UINT32_MAX ? NULL : g->text + g->paths[p];
    for (uint32_t i = 0; i < g->unit_count; i++) {
        const sl_rg_unit *ru = g->units + i;
        sl_unit *u = r->units + i;
        sl_discovery *d = r->discoveries + i;
        base->units[i] = u;
        u->unit = &d->record; u->discovery = d; u->index = i; u->scope = 0; u->site_count = ru->site_count;
        d->record.family = g->text + ru->family; d->record.name = g->text + ru->name;
        d->record.files = r->files + ru->files; d->record.file_count = ru->file_count;
        for (uint32_t f = 0; f < ru->file_count; f++) r->files[ru->files + f] = r->resources + ru->files + f;
        d->record.witnesses = r->witnesses + ru->witnesses; d->record.witness_count = ru->witness_count;
        for (uint32_t w = 0; w < ru->witness_count; w++) r->witnesses[ru->witnesses + w].resource = r->resources + ru->witnesses + w;
        d->dependencies = r->targets + ru->deps; d->dependency_count = ru->dep_count;
        for (uint32_t k = 0; k < ru->dep_count; k++) r->targets[ru->deps + k] = r->units + g->targets[ru->deps + k];
        d->sites = r->sites + ru->sites;
        for (uint32_t j = 0; j < ru->site_count; j++) {
            const sl_rg_edge *e = g->edges + ru->sites + j;
            r->sites[ru->sites + j] = (sl_site){NULL, e->slot == UINT32_MAX ? NULL : g->text + e->slot, r->units + e->target};
        }
        d->implicit = r->implicits + ru->implicits; d->implicit_count = ru->implicit_count;
        for (uint32_t j = 0; j < ru->implicit_count; j++) {
            const sl_rg_edge *e = g->edges + ru->implicits + j;
            r->implicits[ru->implicits + j] = (sl_implicit){e->slot == UINT32_MAX ? NULL : g->text + e->slot, r->units + e->target, 0};
        }
        d->discovered_done = 1;
    }
    return base;
}
/* What the stock reverse base depends on: the catalog, the native readers, the renderer and the adapters the
 * product wires (their behaviour is the product's; a record kept across processes is also keyed by the product
 * and game images by its keeper). 0 when the identities are unavailable. */
static int sl_reverse_graph_key(sh_package_source_link *s, const sh_package_source_root_plan *plan,
    sh_decl_registry_source registry, unsigned char key[32])
{
    struct { char magic[32]; unsigned char catalog[32], native[32]; int32_t renderer; uint32_t flags; } material;
    sh_package_file_identity identity;
    memset(&material, 0, sizeof(material));
    memcpy(material.magic, "snapmap-plus reverse graph 1", 28);
    if (!s->catalog || !sh_resource_catalog_digest(s->catalog, material.catalog) || !sl_native_digest(s, material.native)) return 0;
    material.renderer = s->native.shader ? (int32_t)s->native.shader->renderer : -1;
    material.flags = (s->native.prepare != NULL) | (s->native.admit != NULL) << 1 | (s->native.implicit != NULL) << 2 |
        (registry.scripts != NULL) << 3 | (sl_canonicalizer_of(s->scopes[plan->baseline_scope]) == sh_package_engine_path) << 4 |
        (s->native.shader && s->native.shader->program != NULL) << 5;
    if (!sh_package_bytes_identity(&material, sizeof(material), &identity)) return 0;
    memcpy(key, identity.digest, 32); return 1;
}
/* The record form (sh_package_source_memo_reverse_export): magic, format, key, seven counts, the arrays, the text,
 * then the SHA-256 of everything before it. Little-endian 32-bit fields. */
static const char g_sl_rg_magic[28] = "snapmap-plus reverse record";
static void sl_rg_put(unsigned char *at, uint32_t value)
{ for (int i = 0; i < 4; i++) at[i] = (unsigned char)(value >> (8 * i)); }
static uint32_t sl_rg_get(const unsigned char *at)
{ return (uint32_t)at[0] | (uint32_t)at[1] << 8 | (uint32_t)at[2] << 16 | (uint32_t)at[3] << 24; }
static int sl_reverse_graph_encode(const sl_reverse_graph *g, unsigned char **bytes, size_t *length)
{
    size_t units = (size_t)g->unit_count * 13 * 4, targets = (size_t)g->target_count * 4, paths = (size_t)g->path_count * 4;
    size_t edges = (size_t)g->edge_count * 8, gaps = (size_t)g->gap_count * 12;
    size_t head = sizeof(g_sl_rg_magic) + 4 + 32 + 7 * 4;
    size_t total = head + units + targets + paths + edges + gaps + g->text_length + 32;
    unsigned char *out = malloc(total), *at;
    sh_package_file_identity identity;
    if (!out) return 0;
    at = out;
    memcpy(at, g_sl_rg_magic, sizeof(g_sl_rg_magic)); at += sizeof(g_sl_rg_magic);
    sl_rg_put(at, 1); at += 4;
    memcpy(at, g->key, 32); at += 32;
    sl_rg_put(at, g->unit_count); sl_rg_put(at + 4, g->root_count); sl_rg_put(at + 8, g->gap_count);
    sl_rg_put(at + 12, g->target_count); sl_rg_put(at + 16, g->edge_count); sl_rg_put(at + 20, g->path_count);
    sl_rg_put(at + 24, g->text_length); at += 28;
    for (uint32_t i = 0; i < g->unit_count; i++) {
        const uint32_t *fields = (const uint32_t *)(g->units + i);
        for (int k = 0; k < 13; k++) { sl_rg_put(at, fields[k]); at += 4; }
    }
    for (uint32_t i = 0; i < g->target_count; i++) { sl_rg_put(at, g->targets[i]); at += 4; }
    for (uint32_t i = 0; i < g->path_count; i++) { sl_rg_put(at, g->paths[i]); at += 4; }
    for (uint32_t i = 0; i < g->edge_count; i++) { sl_rg_put(at, g->edges[i].slot); sl_rg_put(at + 4, g->edges[i].target); at += 8; }
    for (uint32_t i = 0; i < g->gap_count; i++) {
        sl_rg_put(at, g->gaps[i].family); sl_rg_put(at + 4, g->gaps[i].name); sl_rg_put(at + 8, g->gaps[i].reason); at += 12;
    }
    memcpy(at, g->text, g->text_length); at += g->text_length;
    if (!sh_package_bytes_identity(out, (size_t)(at - out), &identity)) { free(out); return 0; }
    memcpy(at, identity.digest, 32);
    *bytes = out; *length = total; return 1;
}
static sl_reverse_graph *sl_reverse_graph_decode(const unsigned char *bytes, size_t length)
{
    size_t head = sizeof(g_sl_rg_magic) + 4 + 32 + 7 * 4;
    sh_package_file_identity identity;
    sl_reverse_graph *g = NULL;
    const unsigned char *at;
    if (!bytes || length < head + 32 || memcmp(bytes, g_sl_rg_magic, sizeof(g_sl_rg_magic)) ||
        sl_rg_get(bytes + sizeof(g_sl_rg_magic)) != 1) return NULL;
    if (!sh_package_bytes_identity(bytes, length - 32, &identity) || memcmp(identity.digest, bytes + length - 32, 32)) return NULL;
    g = calloc(1, sizeof(*g)); if (!g) return NULL;
    at = bytes + sizeof(g_sl_rg_magic) + 4;
    memcpy(g->key, at, 32); at += 32;
    g->unit_count = sl_rg_get(at); g->root_count = sl_rg_get(at + 4); g->gap_count = sl_rg_get(at + 8);
    g->target_count = sl_rg_get(at + 12); g->edge_count = sl_rg_get(at + 16); g->path_count = sl_rg_get(at + 20);
    g->text_length = sl_rg_get(at + 24); at += 28;
    {
        size_t body = length - 32 - head;
        size_t need = (size_t)g->unit_count * 52 + (size_t)g->target_count * 4 + (size_t)g->path_count * 4 +
            (size_t)g->edge_count * 8 + (size_t)g->gap_count * 12 + (size_t)g->text_length;
        if (need != body || !g->text_length) { free(g); return NULL; }
    }
    g->units = calloc(g->unit_count ? g->unit_count : 1, sizeof(*g->units));
    g->targets = calloc(g->target_count ? g->target_count : 1, sizeof(*g->targets));
    g->paths = calloc(g->path_count ? g->path_count : 1, sizeof(*g->paths));
    g->edges = calloc(g->edge_count ? g->edge_count : 1, sizeof(*g->edges));
    g->gaps = calloc(g->gap_count ? g->gap_count : 1, sizeof(*g->gaps));
    g->text = malloc(g->text_length);
    if (!g->units || !g->targets || !g->paths || !g->edges || !g->gaps || !g->text) { sl_reverse_graph_free(g); return NULL; }
    for (uint32_t i = 0; i < g->unit_count; i++) {
        uint32_t *fields = (uint32_t *)(g->units + i);
        for (int k = 0; k < 13; k++) { fields[k] = sl_rg_get(at); at += 4; }
    }
    for (uint32_t i = 0; i < g->target_count; i++) { g->targets[i] = sl_rg_get(at); at += 4; }
    for (uint32_t i = 0; i < g->path_count; i++) { g->paths[i] = sl_rg_get(at); at += 4; }
    for (uint32_t i = 0; i < g->edge_count; i++) { g->edges[i].slot = sl_rg_get(at); g->edges[i].target = sl_rg_get(at + 4); at += 8; }
    for (uint32_t i = 0; i < g->gap_count; i++) {
        g->gaps[i].family = sl_rg_get(at); g->gaps[i].name = sl_rg_get(at + 4); g->gaps[i].reason = sl_rg_get(at + 8); at += 12;
    }
    memcpy(g->text, at, g->text_length);
    if (!sl_reverse_graph_valid(g)) { sl_reverse_graph_free(g); return NULL; }
    return g;
}
int sh_package_source_memo_reverse_export(const sh_package_source_memo *memo, unsigned char **bytes, size_t *length)
{
    if (bytes) *bytes = NULL;
    if (length) *length = 0;
    if (!memo || !memo->reverse || !bytes || !length) return 0;
    return sl_reverse_graph_encode(memo->reverse, bytes, length);
}
int sh_package_source_memo_reverse_import(sh_package_source_memo *memo, const unsigned char *bytes, size_t length)
{
    sl_reverse_graph *g = memo ? sl_reverse_graph_decode(bytes, length) : NULL;
    if (!g) return 0;
    sl_reverse_graph_free(memo->reverse); memo->reverse = g; return 1;
}
int sh_package_source_memo_reverse_key(const sh_package_source_memo *memo, unsigned char key[32], size_t *units)
{
    if (!memo || !memo->reverse) return 0;
    if (key) memcpy(key, memo->reverse->key, 32);
    if (units) *units = memo->reverse->unit_count;
    return 1;
}
void sh_package_source_memo_reverse_clear(sh_package_source_memo *memo)
{
    if (!memo) return;
    sl_reverse_graph_free(memo->reverse); memo->reverse = NULL;
}
#ifdef SH_PACKAGE_SOURCE_LINK_TESTING
/* Tests: captures kept, bases rebuilt, and rebuilt bases whose fresh discovery (run beside them when the oracle
 * is on) would have encoded differently. */
static size_t g_sl_reverse_graph_captured, g_sl_reverse_graph_reused, g_sl_reverse_graph_mismatches;
static int g_sl_reverse_graph_oracle = 1;
void sh_package_source_link_test_reverse_graph(size_t *captured, size_t *reused, size_t *mismatches)
{ *captured = g_sl_reverse_graph_captured; *reused = g_sl_reverse_graph_reused; *mismatches = g_sl_reverse_graph_mismatches; }
void sh_package_source_link_test_reverse_graph_oracle(int enabled) { g_sl_reverse_graph_oracle = enabled; }
#endif
/* The fresh stock reverse base: every catalog root (or the plan's candidates) discovered over the baseline. roots
 * receives how many roots the base starts with, edges the dependency edges, incomplete whether a unit was skipped
 * for pending native preparation (the shape is then not the complete one). NULL with s->error set, as before. */
static sh_package_source_link *sl_reverse_discover(sh_package_source_link *s, const sh_package_source_root_plan *plan,
    sh_decl_registry_source registry, uintptr_t reflection, sh_package_compilation *empty, sl_reverse_native *admission,
    size_t *roots, size_t *edge_count, int *incomplete)
{
    sh_package_source_link *base = calloc(1, sizeof(*base));
    *roots = 0; *edge_count = 0; *incomplete = 0;
    if (!base) return NULL;
    base->scopes = calloc(1, sizeof(*base->scopes)); base->readers = calloc(1, sizeof(*base->readers));
    if (!base->scopes || !base->readers) goto failed;
    empty->sources = s->scopes[plan->baseline_scope]->sources;
    empty->canonical_path = s->scopes[plan->baseline_scope]->canonical_path;
    base->scopes[0] = empty; base->scope_count = 1;
    base->observe = 1;
    base->catalog = s->catalog; base->original = sl_read_original_observed; base->original_context = s;
    /* Reverse-only reads must not populate the serving capture with every
     * installed module member. Existing captured originals are copied through
     * their current body-owner reader; other paths use the same immutable
     * original source/catalog. This observing view owns its temporary bytes. */
    base->native.original_view = NULL; base->native.original_view_context = NULL;
    base->error = s->error; base->error_capacity = s->error_capacity;
    base->native.context = admission; base->native.admit = sl_reverse_admit;
    base->native.implicit = sl_reverse_implicit;
    if (s->native.prepare) base->native.prepare = sl_reverse_prepare;
    base->native.shader = s->native.shader;
    base->readers[0] = sh_package_source_reader_open(empty, s->catalog, sl_read_original_observed, s,
        registry, reflection, s->error, s->error_capacity);
    if (!base->readers[0] || (s->native.shader &&
        !sh_package_source_reader_shader(base->readers[0], s->native.shader))) goto failed;
    sh_package_source_reader_original_status(base->readers[0], SL_STATUS ? sl_original_status : NULL);
    /* PaletteBuild enumerates the snapEditorEntityDef manager. Campaign-only
     * entries and generic entity declarations are not additional palette roots.
     * A legacy document instead names its own roots; a name with no stock
     * source is an empty leaf here and is never nominated. */
    if (plan->candidates) for (size_t i = 0; i < plan->candidates->count; i++) {
        const sh_package_reference *root = plan->candidates->items + i;
        if (!root->type || !*root->type || !root->name || !*root->name ||
            !strcmp(root->type, SH_PACKAGE_KEY_FAMILY)) continue;
        if (!sl_find(base, 0, root->type, root->name, 1)) goto failed;
    } else for (size_t i = 0; i < sh_resource_catalog_count(s->catalog); i++) {
        /* A module's catalog card is the editor's presentation of that module
         * (sl_presentation_slot), so its blueprint models make it a root too.
         * A module is placed from the module palette, and its name-derived
         * members (sl_module_members) reach whatever a scope changes below
         * them, so every installed module is a root. */
        const sh_resource_catalog_entry *row = sh_resource_catalog_at(s->catalog, i);
        if (row->archive < 2 && (!strcmp(row->type, "snapeditorentitydef") || !strcmp(row->type, "snapmoduleinfo") ||
            !strcmp(row->type, "snapmodule")) && *row->name && !sl_find(base, 0, row->type, row->name, 1)) goto failed;
    }
    *roots = base->count;
    for (size_t i = 0; i < base->count; i++) {
        sl_unit *unit = base->units[i];
        base->reader_gap = base->preparing = 0;
        int observed = sl_reverse_leaf(unit) || sl_discover_unit(base, unit);
        sl_observed_release(base,unit);
        if (!observed) {
            if (base->preparing) { *incomplete = 1; continue; }
            /* Unreadable originals, allocation and constructor capture failures
             * stop discovery. A stock source beyond the registered readers is a
             * product coverage gap in that root, not in an unrelated package:
             * account it, keep its proven edges and never nominate across it. */
            char reason[768];
            if (base->failed || !base->reader_gap) goto failed;
            snprintf(reason, sizeof(reason), "%s", s->error && *s->error ? s->error : "");
            if (s->error && s->error_capacity) *s->error = 0;
            if (!sl_reverse_gap_add(s, SIZE_MAX, unit, reason, 0)) goto failed;
            unit->discovery->reverse_gap = s->reverse_gap_count < UINT32_MAX ? (uint32_t)s->reverse_gap_count : UINT32_MAX;
        }
        if (unit->discovery->dependency_count > SIZE_MAX - *edge_count) goto failed;
        *edge_count += unit->discovery->dependency_count;
    }
    if (*base->preparation_reason) {
        sl_env_fail(base, base->preparation_reason, NULL); goto failed;
    }
    return base;
failed:
    s->census.reverse_units = base->count;
    sh_package_source_link_close(base);
    return NULL;
}
static int sl_reverse_roots(sh_package_source_link *s, const sh_package_source_root_plan *plan,
    sh_decl_registry_source registry, uintptr_t reflection)
{
    snprintf(s->seed_note, sizeof(s->seed_note), "reverse root discovery");
    sh_package_source_link *base = NULL;
    sl_reverse_index index = {0};
    sh_package_link_node *nodes = NULL;
    sh_package_link_edge *edges = NULL;
    unsigned char *local = NULL, *unchanged = NULL, *complete = NULL, *seen = NULL;
    size_t *stack = NULL;
    size_t roots = 0, edge_count = 0, at = 0;
    int needed = 0, ok = 0, keyed = 0, incomplete = 0;
    unsigned char key[32];
    const sl_reverse_graph *retained = NULL;
    sh_package_compilation empty = {0};
    sl_reverse_native admission = {s, plan ? plan->baseline_scope : SIZE_MAX};
    if (!plan) return 1;
    if (plan->baseline_scope >= s->scope_count)
        return sl_env_fail(s, "reverse authoring roots require an original-only scope", NULL);
    /* A saved derived root need not have an authored declaration file. It must
     * still bind in exactly the recorded source context, never current local. */
    if (plan->recorded) for (size_t i = 0; i < plan->recorded->binding_count; i++) {
        const sh_package_provenance_binding *binding = plan->recorded->bindings + i;
        /* A registration key names no resource (package_keys.h). */
        if (!strcmp(binding->family, SH_PACKAGE_KEY_FAMILY)) continue;
        for (size_t j = 0; j < binding->context_count; j++) {
            size_t scope = binding->contexts[j];
            if (scope >= plan->recorded->context_count || scope >= s->scope_count ||
                !sl_find(s, scope, binding->family, binding->original, 1)) return 0;
        }
    }
    if (plan->discover) for (size_t scope = 0; scope < s->scope_count; scope++) {
        if (!plan->discover[scope]) continue;
        for (size_t f = 0; f < s->scopes[scope]->resource_count; f++)
            if (sh_package_owners_count(&s->scopes[scope]->resources[f].gameplay_owners)) needed = 1;
    }
    if (!needed || (!plan->candidates && !sh_resource_catalog_count(s->catalog))) return 1;
    /* The retained reverse graph (above): only for the catalog's roots, with the memo borrowed and the caller's
     * assertion that the stock originals are the catalog's and the installation's. */
    if (!plan->candidates && s->native.reverse_retain && s->memo && !s->reverse_gap_count)
        keyed = sl_reverse_graph_key(s, plan, registry, key);
    if (keyed && s->memo->reverse && !memcmp(s->memo->reverse->key, key, 32)) retained = s->memo->reverse;
#ifdef SH_PACKAGE_SOURCE_LINK_TESTING
    if (retained && g_sl_reverse_graph_oracle) {
        /* The oracle: the fresh discovery beside the rebuilt base must encode to the same bytes. The reader gaps
         * it accounts in s are dropped again, so the replay below accounts them as the sole discovery would. */
        size_t fresh_roots = 0, fresh_edges = 0, gaps_before = s->reverse_gap_count; int fresh_incomplete = 0;
        sh_package_compilation fresh_empty = {0};
        sh_package_source_link *fresh = sl_reverse_discover(s, plan, registry, reflection, &fresh_empty, &admission,
            &fresh_roots, &fresh_edges, &fresh_incomplete);
        sl_reverse_graph *shape = fresh && !fresh_incomplete ? sl_reverse_capture(s, fresh, fresh_roots, key) : NULL;
        unsigned char *a = NULL, *b = NULL; size_t a_length = 0, b_length = 0;
        if (!shape || !sl_reverse_graph_encode(shape, &a, &a_length) || !sl_reverse_graph_encode(retained, &b, &b_length) ||
            a_length != b_length || memcmp(a, b, a_length)) g_sl_reverse_graph_mismatches++;
        free(a); free(b); sl_reverse_graph_free(shape);
        if (fresh) sh_package_source_link_close(fresh);
        if (s->error && s->error_capacity) *s->error = 0;
        s->failed = 0;
        while (s->reverse_gap_count > gaps_before) {
            sh_package_reverse_gap *gap = s->reverse_gaps + --s->reverse_gap_count;
            free((char *)gap->family); free((char *)gap->name); free((char *)gap->reason);
        }
    }
#endif
    if (retained) {
        base = sl_reverse_rebuild(retained);
        if (!base) goto done;
        roots = retained->root_count; edge_count = retained->target_count;
        /* The reader gaps the discovery accounted, in its order, so each unit's gap index is the one it had. */
        for (uint32_t i = 0; i < retained->unit_count; i++) {
            const sl_rg_unit *ru = retained->units + i;
            if (!ru->gap) continue;
            if (!sl_reverse_gap_add(s, SIZE_MAX, base->units[i], retained->text + retained->gaps[ru->gap - 1].reason, 0)) goto done;
            base->units[i]->discovery->reverse_gap = s->reverse_gap_count < UINT32_MAX ? (uint32_t)s->reverse_gap_count : UINT32_MAX;
        }
        s->census.reverse_retained = 1;
#ifdef SH_PACKAGE_SOURCE_LINK_TESTING
        g_sl_reverse_graph_reused++;
#endif
    } else {
        base = sl_reverse_discover(s, plan, registry, reflection, &empty, &admission, &roots, &edge_count, &incomplete);
        if (!base) goto done;
        if (keyed && !incomplete) {
            sl_reverse_graph *shape = sl_reverse_capture(s, base, roots, key);
            if (shape) {
                sl_reverse_graph_free(s->memo->reverse); s->memo->reverse = shape;
#ifdef SH_PACKAGE_SOURCE_LINK_TESTING
                g_sl_reverse_graph_captured++;
#endif
            }
        }
    }
    if (base->count > SIZE_MAX / sizeof(*nodes) || base->count > SIZE_MAX / sizeof(*stack) ||
        edge_count > SIZE_MAX / sizeof(*edges)) goto done;
    nodes = calloc(base->count ? base->count : 1, sizeof(*nodes));
    edges = calloc(edge_count ? edge_count : 1, sizeof(*edges));
    local = malloc(base->count ? base->count : 1); unchanged = malloc(base->count ? base->count : 1);
    complete = malloc(base->count ? base->count : 1); seen = malloc(base->count ? base->count : 1);
    stack = malloc((base->count ? base->count : 1) * sizeof(*stack));
    if (!nodes || !edges || !local || !unchanged || !complete || !seen || !stack) goto done;
    for (size_t i = 0; i < base->count; i++) {
        const sl_unit *unit = base->units[i];
        nodes[i].edges = edges + at; nodes[i].edge_count = unit->discovery->dependency_count;
        for (size_t j = 0; j < unit->discovery->dependency_count; j++) edges[at++].target = unit->discovery->dependencies[j]->index;
        local[i] = !unit->discovery->reverse_gap;
    }
    /* A root is completely observed only when no reachable unit stopped early. */
    if (!sh_package_link_originals(nodes, base->count, local, complete, s->error, s->error_capacity)) goto done;
    if (!sl_reverse_index_open(s, base, plan, &index)) {
        sl_env_fail(s, "cannot index reverse dependency changes", NULL); goto done;
    }
    for (size_t scope = 0; scope < s->scope_count; scope++) if (plan->discover[scope]) {
        /* A player hands web changes globally (the library composes it under
         * its stock name), so reaching it gives a stock root no variant. A scope
         * that changes no base unit reaches no root. */
        if (!sl_reverse_scope_changes(s, base, &index, scope, local)) continue;
        /* Reverse propagation is candidate discovery only. Final scoped graph
         * equality/certification still decides whether a private root exists. */
        if (!sh_package_link_originals(nodes, base->count, local, unchanged, s->error, s->error_capacity)) goto done;
        for (size_t i = 0; i < roots; i++) if (!unchanged[i]) {
            const sl_unit *unit = base->units[i];
            if (!complete[i]) {
                /* This scope changes a stock entry whose remaining closure the
                 * readers cannot bind faithfully. Expose no guessed variant;
                 * attribute the missing coverage to this scope and continue. */
                const sl_unit *gap = sl_reverse_gap_reached(base, i, seen, stack);
                if (!sl_reverse_gap_add(s, scope, unit, gap && gap->discovery->reverse_gap ?
                    s->reverse_gaps[gap->discovery->reverse_gap - 1].reason : NULL, 0)) goto done;
                continue;
            }
            if (!strcmp(unit->unit->family, "snapmoduleinfo") && !sl_card_presentation_reach(unit, unchanged)) {
                /* Its module's variant carries such a change (a module root
                 * reaches the card); a card no installed module has cannot be
                 * carried by one. */
                if (sl_card_module(s, unit->unit->name)) continue;
                if (!sl_reverse_gap_add(s, scope, unit,
                    "it changes outside the card's blueprint models, and no installed module carries the card", 1)) goto done;
                continue;
            }
            if (!strcmp(unit->unit->family, "snapmodule") && !sl_module_variant_reach(unit, local, unchanged)) continue;
            size_t before = s->count;
            sl_unit *root = sl_find(s, scope, unit->unit->family, unit->unit->name, 1);
            if (!root || !sl_find(s, plan->baseline_scope, unit->unit->family, unit->unit->name, 1)) goto done;
            if (s->count > before && s->units[before] == root) root->discovery->nominated = 1;
        }
    }
    ok = 1;
done:
    if (!ok) {
        char detail[768]; snprintf(detail, sizeof(detail), "%s", s->error && *s->error ? s->error : "allocation failed");
        if (s->error && s->error_capacity) snprintf(s->error, s->error_capacity,
            "baseline palette reverse dependency coverage is incomplete: %s", detail);
        s->failed = 1;
    }
    sl_reverse_index_close(&index, s->scope_count);
    free(nodes); free(edges); free(local); free(unchanged); free(complete); free(seen); free(stack);
    if (base) s->census.reverse_units = base->count;
    sh_package_source_link_close(base); return ok;
}
static int sl_unit_order(const void *a, const void *b)
{
    const sl_unit *x = *(sl_unit *const *)a, *y = *(sl_unit *const *)b;
    if (x->scope != y->scope) return x->scope < y->scope ? -1 : 1;
    int order = strcmp(x->unit->family, y->unit->family);
    return order ? order : strcmp(x->unit->name, y->unit->name);
}
/* With shared stock, each unit sorts at the place its first copy would have had without sharing: a shared stock
 * unit at the earliest scope that binds it (place, by the unit's pre-sort index), every other unit at its own scope.
 * The linker's classes then have the same first members and representatives as without sharing, so what a class
 * publishes takes its first input from the same copy (R-344 stage A). */
static int sl_unit_place(void *context, const void *a, const void *b)
{
    const uint32_t *place = context;
    const sl_unit *x = *(sl_unit *const *)a, *y = *(sl_unit *const *)b;
    uint32_t p = place[x->index], q = place[y->index];
    if (p != q) return p < q ? -1 : 1;
    int order = strcmp(x->unit->family, y->unit->family);
    if (!order) order = strcmp(x->unit->name, y->unit->name);
    return order ? order : x->scope < y->scope ? -1 : x->scope > y->scope;
}
static int sl_file_order(const void *a, const void *b)
{ return strcmp((*(const sh_compiled_resource *const *)a)->engine_path, (*(const sh_compiled_resource *const *)b)->engine_path); }

/* Editor presentation inherits the scope of the gameplay it can reach. This
 * uses the exact reader edges already discovered above, including inherited
 * definitions and cycles. It is not a package-ID or filename classification.
 * A shared result is only a composition eligibility decision, never proof that
 * the native identity can be retained or a map does not need a dependency. */
/* The frozen form of every unit's sites, in one capture-owned block each: (slot, unit index) link edges in site
 * order, and each file's maximal run of them. Units are already indexed. 0 when a block cannot be kept. */
static int sl_freeze_sites(sh_package_source_link *s)
{
    size_t sites = 0, runs = 0;
    for (size_t i = 0; i < s->count; i++) {
        const sl_unit *unit = s->units[i];
        if (unit->site_count > SIZE_MAX - sites) return 0;
        sites += unit->site_count;
        for (size_t j = 0; j < unit->site_count; j++) runs += !j || unit->discovery->sites[j].file != unit->discovery->sites[j - 1].file;
    }
    if (sites > SIZE_MAX / sizeof(*s->site_edge_block) || runs > SIZE_MAX / sizeof(*s->site_run_block)) return 0;
    if (sites && (SL_REFUSED(g_sl_block_refusal) ||
        !(s->site_edge_block = malloc(sites * sizeof(*s->site_edge_block))))) return 0;
    if (runs && (SL_REFUSED(g_sl_block_refusal) ||
        !(s->site_run_block = malloc(runs * sizeof(*s->site_run_block))))) return 0;
    sh_package_link_edge *edge = s->site_edge_block;
    sl_site_run *run = s->site_run_block;
    for (size_t i = 0; i < s->count; i++) {
        sl_unit *unit = s->units[i];
        if (!unit->site_count) continue;
        unit->site_edges = edge; unit->site_runs = run;
        for (size_t j = 0; j < unit->site_count; j++) {
            *edge++ = (sh_package_link_edge){unit->discovery->sites[j].slot, unit->discovery->sites[j].target->index};
            if (j && unit->discovery->sites[j].file == unit->discovery->sites[j - 1].file) run[-1].end = j + 1;
            else *run++ = (sl_site_run){unit->discovery->sites[j].file, j + 1};
        }
    }
    return 1;
}
#if defined(SH_PACKAGE_LIBRARY_TESTING) || defined(SH_PACKAGE_SOURCE_LINK_TESTING)
/* Tests: every discovered file's frozen range against the scan of its discovery sites (the range before e055). */
static size_t g_sl_site_ranges_checked, g_sl_site_ranges_mismatched;
void sh_package_source_link_test_site_ranges(size_t *checked, size_t *mismatched)
{ *checked = g_sl_site_ranges_checked; *mismatched = g_sl_site_ranges_mismatched; }
static size_t g_sl_discovery_released, g_sl_discovery_kept;
void sh_package_source_link_test_discovery(size_t *released, size_t *kept)
{ *released = g_sl_discovery_released; *kept = g_sl_discovery_kept; }
static void sl_site_ranges_check(const sh_package_source_link *s)
{
    for (size_t i = 0; i < s->count; i++) {
        const sl_unit *unit = s->units[i];
        for (size_t k = 0; k < unit->discovered_count; k++) {
            const sh_compiled_resource *file = unit->discovered[k];
            size_t begin = 0, end, frozen_begin, frozen_end;
            while (begin < unit->site_count && unit->discovery->sites[begin].file != file) begin++;
            for (end = begin; end < unit->site_count && unit->discovery->sites[end].file == file; end++);
            sl_site_range(unit, file, &frozen_begin, &frozen_end);
            g_sl_site_ranges_checked++;
            if (frozen_end - frozen_begin != end - begin || (end > begin && frozen_begin != begin)) g_sl_site_ranges_mismatched++;
        }
    }
}
#endif
/* The freeze ends discovery: no reader follows a unit's dependencies or discovery sites after sl_shared, or reads
 * the rest of its discovery state after the freeze has kept what frozen readers need (sl_unit.discovery is NULL). */
static void sl_discovery_release(sh_package_source_link *s)
{
#if defined(SH_PACKAGE_LIBRARY_TESTING) || defined(SH_PACKAGE_SOURCE_LINK_TESTING)
    sl_site_ranges_check(s);
#endif
    for (size_t i = 0; i < s->count; i++) {
        sl_unit *unit = s->units[i];
        free(unit->discovery->seed); free(unit->discovery->implicit); free(unit->discovery->availability_semantics);
        unit->discovery = NULL;
    }
    sl_arena_free(&s->dependency_arena); sl_arena_free(&s->site_arena); sl_arena_free(&s->discovery_arena);
#if defined(SH_PACKAGE_LIBRARY_TESTING) || defined(SH_PACKAGE_SOURCE_LINK_TESTING)
    int kept = s->dependency_arena.blocks || s->site_arena.blocks || s->discovery_arena.blocks;
    for (size_t i = 0; i < s->count; i++) kept |= s->units[i]->discovery != NULL;
    g_sl_discovery_released += s->count; g_sl_discovery_kept += kept != 0;
#endif
}
static int sl_shared(sh_package_source_link *s)
{
    sh_package_link_node *nodes = NULL;
    sh_package_link_edge *edges = NULL;
    size_t edge_count = 0, at = 0;
    int ok = 0;
    for (size_t i = 0; i < s->count; i++) {
        if (s->units[i]->discovery->dependency_count > SIZE_MAX - edge_count) goto done;
        edge_count += s->units[i]->discovery->dependency_count;
    }
    if (s->count > SIZE_MAX / sizeof(*nodes) || edge_count > SIZE_MAX / sizeof(*edges)) goto done;
    nodes = calloc(s->count ? s->count : 1, sizeof(*nodes));
    edges = calloc(edge_count ? edge_count : 1, sizeof(*edges));
    s->shared = calloc(s->count ? s->count : 1, 1);
    if (!nodes || !edges || !s->shared) goto done;
    for (size_t i = 0; i < s->count; i++) {
        const sl_unit *unit = s->units[i];
        s->shared[i] = 1;
        for (size_t f = 0; f < unit->unit->file_count; f++)
            if (sh_package_owners_count(&unit->unit->files[f]->gameplay_owners)) s->shared[i] = 0;
        for (size_t f = 0; f < unit->unit->witness_count; f++)
            if (sh_package_owners_count(&unit->unit->witnesses[f].resource->gameplay_owners)) s->shared[i] = 0;
        if (unit->unit->implicit_gameplay_owners && sh_package_owners_count(unit->unit->implicit_gameplay_owners)) s->shared[i] = 0;
        nodes[i].edges = edges + at; nodes[i].edge_count = unit->discovery->dependency_count;
        for (size_t d = 0; d < unit->discovery->dependency_count; d++)
            edges[at++].target = unit->discovery->dependencies[d]->index;
    }
    ok = sh_package_link_originals(nodes, s->count, s->shared, s->shared, s->error, s->error_capacity);
done:
    free(nodes); free(edges);
    if (!ok) return sl_env_fail(s, "cannot classify scoped source dependencies", NULL);
    return 1;
}

/* A unit carries content its scope supplied, not only product built-ins
 * (which have no input files) or captured originals. */
static int sl_unit_supplied(const sl_unit *unit)
{
    for (size_t i = 0; i < unit->unit->file_count; i++) if (unit->unit->files[i]->source_count) return 1;
    for (size_t i = 0; i < unit->unit->witness_count; i++)
        if (unit->unit->witnesses[i].resource->source_count) return 1;
    return 0;
}
static int sl_scope_supplies(const sh_package_source_link *s, size_t scope)
{
    const sh_package_compilation *compiled = scope < s->scope_count ? s->scopes[scope] : NULL;
    for (size_t i = 0; compiled && i < compiled->resource_count; i++)
        if (compiled->resources[i].source_count) return 1;
    return 0;
}
/* A changed editor declaration with no gameplay contribution may be the one
 * shared object every scope reaches by its native name (package_library.c,
 * stable names): its content then does not change what binds it. */
static int sl_stable_candidate(const sl_unit *unit)
{
    const char *family = unit->unit->family;
    if (strcmp(family, "entitydef") && strncmp(family, "snapeditor", 10) && strncmp(family, "snappropertyinspector", 21))
        return 0;
    for (size_t i = 0; i < unit->unit->file_count; i++)
        if (sh_package_owners_count(&unit->unit->files[i]->gameplay_owners)) return 0;
    return 1;
}
/* Edge e of unit, over its dependencies, then its sites, then its implicit
 * requests; slot is the site's field path, or NULL. */
static const sl_unit *sl_edge(const sl_unit *unit, size_t e, const char **slot)
{
    if (slot) *slot = NULL;
    if (e < unit->discovery->dependency_count) return unit->discovery->dependencies[e];
    e -= unit->discovery->dependency_count;
    if (e < unit->site_count) { if (slot) *slot = unit->discovery->sites[e].slot; return unit->discovery->sites[e].target; }
    return unit->discovery->implicit[e - unit->site_count].target;
}
/* A module's catalog card is found by its module's name (the module wrapper
 * loader resolves moduleInfo by name), so it keeps that name: one card per
 * module. Its blueprint floors' snapFloor_t.floorModel (idStrRenderModel_Preview)
 * is read only by the editor: the SnapMap Simple Geo builder (VK 0x140d35540 /
 * GL 0x140d34c00) loads each floor model from the card's floors list, and is
 * reached only from the editor's module draw (VK 0x140521d90 -> 0x1405e7380 ->
 * 0x140d36f20 / GL 0x1405216d0 -> 0x1405e6f70 -> 0x140d365e0); the field name
 * is referenced only by its reflection table. A change reaching the card only
 * through those models is the editor's presentation of that module, one slot
 * resolved deterministically across packages (README "Requirements, strings
 * and exposure"; cosmetic changes alone do not clone the module). No other
 * card field has such consumer evidence. A card qualifies while its own bytes
 * are the original's and every changed binding leaves it through floorModel. */
static int sl_presentation_field(const char *slot)
{
    static const char floors[] = "edit.floors.item[", model[] = "].floorModel";
    size_t n = slot ? strlen(slot) : 0, f = sizeof(floors) - 1, m = sizeof(model) - 1;
    if (!slot) return 0;
    if (n <= f + m || strncmp(slot, floors, f) || strcmp(slot + n - m, model)) return 0;
    for (size_t i = f; i < n - m; i++) if (slot[i] < '0' || slot[i] > '9') return 0;
    return 1;
}
static int sl_presentation_slot(const sl_unit *unit, const unsigned char *changed)
{
    size_t edges = unit->discovery->dependency_count + unit->site_count + unit->discovery->implicit_count;
    int presented = 0;
    if (strcmp(unit->unit->family, "snapmoduleinfo") || unit->unit->original_binding != 1 || unit->unit->file_count != 1) return 0;
    for (size_t e = unit->discovery->dependency_count; e < edges; e++) {
        const char *slot; const sl_unit *target = sl_edge(unit, e, &slot);
        if (!target || !changed[target->index] || sl_stable_candidate(target)) continue;
        if (!sl_presentation_field(slot)) return 0;
        presented = 1;
    }
    /* A dependency is also a site or an implicit request; one that is neither
     * has no field this rule can classify. */
    for (size_t d = 0; d < unit->discovery->dependency_count; d++) {
        const sl_unit *target = unit->discovery->dependencies[d];
        int seen = 0;
        if (!target || !changed[target->index] || sl_stable_candidate(target)) continue;
        for (size_t e = unit->discovery->dependency_count; !seen && e < edges; e++) seen = sl_edge(unit, e, NULL) == target;
        if (!seen) return 0;
    }
    return presented;
}
/* What a changed binding reaches is a different binding too (README Compiler
 * contract 5): it takes a private identity, which needs a verified relocation
 * rule, and its consumers' reference sites are rewritten. A product-generated
 * unit no reference in its scope names is reached only by the name its
 * consumers derive, so a different binding of it has no reachable private
 * identity without a module variant. Refused here, a unit names the scope
 * responsible instead of failing every package at assignment. */
static const char *sl_module_role(const sl_unit *unit, const sl_unit *target)
{
    for (size_t j = 0; j < unit->discovery->implicit_count; j++) {
        const char *slot = unit->discovery->implicit[j].slot;
        if (unit->discovery->implicit[j].target == target && slot && !strncmp(slot, "module/", 7)) return slot + 7;
    }
    return NULL;
}

static int sl_changed_bindings(sh_package_source_link *s)
{
    size_t n = s->count ? s->count : 1;
    unsigned char *changed = calloc(n, 1), *referenced = calloc(n, 1), *module_in = calloc(n, 1), *other_in = calloc(n, 1);
    const sl_unit **cause = calloc(n, sizeof(*cause));
    size_t *module_of = malloc(n * sizeof(*module_of));
    int ok = 1;
    if (!changed || !referenced || !cause || !module_in || !other_in || !module_of) {
        free(changed); free(referenced); free(cause); free(module_in); free(other_in); free(module_of);
        return sl_env_fail(s, "cannot certify changed bindings", NULL);
    }
    for (size_t i = 0; i < s->count; i++) module_of[i] = SIZE_MAX;
    for (size_t i = 0; i < s->count; i++) {
        const sl_unit *unit = s->units[i];
        size_t edges = unit->discovery->dependency_count + unit->site_count + unit->discovery->implicit_count;
        if (unit->unit->file_count && !unit->unit->original_binding) { changed[i] = 1; cause[i] = unit; }
        for (size_t e = 0; e < edges; e++) {
            const sl_unit *target = sl_edge(unit, e, NULL);
            if (target && target != unit) referenced[target->index] = 1;
        }
        for (size_t d = 0; d < unit->discovery->dependency_count; d++) {
            const sl_unit *target = unit->discovery->dependencies[d];
            const char *role = target && target != unit ? sl_module_role(unit, target) : NULL;
            if (!target || target == unit) continue;
            if (role) {
                module_in[target->index] = 1;
                if (!strncmp(role, "card/", 5)) module_of[target->index] = i;
            } else other_in[target->index] = 1;
        }
    }
    for (int again = 1; again;) {
        again = 0;
        for (size_t i = 0; i < s->count; i++) {
            const sl_unit *unit = s->units[i];
            size_t edges = unit->discovery->dependency_count + unit->site_count + unit->discovery->implicit_count;
            for (size_t e = 0; !changed[i] && e < edges; e++) {
                const sl_unit *target = sl_edge(unit, e, NULL);
                if (!target || target == unit || !changed[target->index] || sl_stable_candidate(target)) continue;
                /* A card that stays the module's presentation does not change it. */
                if (module_of[target->index] == i && sl_presentation_slot(target, changed)) continue;
                changed[i] = 1; cause[i] = cause[target->index]; again = 1;
            }
        }
    }
    for (size_t i = 0; ok && i < s->count; i++) {
        sl_unit *unit = s->units[i];
        char reason[768], rule[512];
        int card = !strcmp(unit->unit->family, "snapmoduleinfo");
        unit->presentation = 0;
        /* A card's presentation needs its module otherwise unchanged; a module
         * variant carries the card as it is changed instead. */
        if (changed[i] && unit->unit->file_count && sl_presentation_slot(unit, changed) &&
            (module_of[i] == SIZE_MAX || !changed[module_of[i]])) unit->presentation = 1;
        if (card && module_of[i] != SIZE_MAX && (!changed[i] || unit->presentation)) unit->unit->stable_name = 1;
        unit->unit->carried = module_in[i] && !other_in[i] && !unit->presentation;
        if (!changed[i] || !unit->unit->file_count || unit->presentation || unit->unit->carried) continue;
        if (card) {
            if (cause[i] == unit)
                snprintf(reason, sizeof(reason), "a module's catalog card is found by its module's name; a change to its "
                    "own fields is carried by a variant of that module, and no installed module has this card");
            else
                snprintf(reason, sizeof(reason), "a module's catalog card is found by its module's name; changed %s %s "
                    "reached outside its blueprint models is carried by a variant of that module, and no installed module "
                    "has this card", cause[i]->unit->family, cause[i]->unit->name);
        } else if (unit->discovery->product && !referenced[i] && strcmp(unit->unit->family, "snapmodule")) {
            /* A module is placed by its own name; a changed one takes a variant. */
            if (cause[i] == unit)
                snprintf(reason, sizeof(reason), "product-generated resource differs from the product's own output; its "
                    "consumers derive this native name from a module this scope does not reach");
            else
                snprintf(reason, sizeof(reason), "product-generated resource binds changed %s %s; its consumers derive this "
                    "native name from a module this scope does not reach", cause[i]->unit->family, cause[i]->unit->name);
        } else if (((!strcmp(unit->unit->family, "file") && !unit->unit->file_identities) ||
            !strcmp(unit->unit->family, "snapmodule")) &&
            !sh_package_native_names_relocatable(unit->unit, rule, sizeof(rule))) {
            /* A map's light probe, which this capture reads, has no relocation
             * rule outside a module: a changed one is refused for its own scope
             * here rather than for every package at assignment. The rule
             * already names the unit. */
            if (cause[i] == unit) snprintf(reason, sizeof(reason), "%s", rule);
            else snprintf(reason, sizeof(reason), "%s (it binds changed %s %s)", rule, cause[i]->unit->family, cause[i]->unit->name);
            s->failed_unit = unit; ok = sl_fail(s, reason, NULL); continue;
        } else continue;
        s->failed_unit = unit; ok = sl_fail(s, reason, unit->unit->name);
    }
    free(changed); free(referenced); free(cause); free(module_in); free(other_in); free(module_of); return ok;
}
/* Classify a failed closure by the content that reached it (see the header).
 * The chain follows first requesters from the failing unit back to its seed. */
static void sl_attribute(const sh_package_source_link *s, sh_package_source_failure *failure)
{
    const sl_unit *failed = s->failed_unit, *supplier = NULL, *root = NULL;
    size_t used = 0;
    failure->kind = SH_PACKAGE_SOURCE_FAILURE_NONE; failure->scope = SIZE_MAX;
    failure->family[0] = failure->name[0] = failure->chain[0] = 0;
    if (s->environment) { failure->kind = SH_PACKAGE_SOURCE_FAILURE_ENVIRONMENT; return; }
    if (!failed) {
        if (s->failed_scope != SIZE_MAX && sl_scope_supplies(s, s->failed_scope)) {
            failure->kind = SH_PACKAGE_SOURCE_FAILURE_COVERAGE; failure->scope = s->failed_scope;
        }
        return;
    }
    snprintf(failure->family, sizeof(failure->family), "%s", failed->unit->family);
    snprintf(failure->name, sizeof(failure->name), "%s", failed->unit->name);
    for (const sl_unit *unit = failed; unit; unit = unit->discovery->parent) {
        if (!supplier && sl_unit_supplied(unit)) supplier = unit;
        root = unit;
        if (used + 1 < sizeof(failure->chain)) {
            int n = snprintf(failure->chain + used, sizeof(failure->chain) - used, "%s%s %s",
                unit == failed ? "" : " <- ", unit->unit->family, unit->unit->name);
            if (n < 0 || (size_t)n >= sizeof(failure->chain) - used) {
                /* Keep the nearest requesters; mark the omitted seed side. */
                size_t keep = sizeof(failure->chain) - 8;
                if (used > keep) used = keep;
                snprintf(failure->chain + used, sizeof(failure->chain) - used, " <- ...");
                used = sizeof(failure->chain);
            } else used += (size_t)n;
        }
    }
    if (root && root->discovery->seed && used + 1 < sizeof(failure->chain)) {
        int n = snprintf(failure->chain + used, sizeof(failure->chain) - used, " [%s]", root->discovery->seed);
        used = n < 0 || (size_t)n >= sizeof(failure->chain) - used ? sizeof(failure->chain) : used + (size_t)n;
    }
    if (supplier) {
        failure->kind = supplier == failed && s->malformed ?
            SH_PACKAGE_SOURCE_FAILURE_SOURCE : SH_PACKAGE_SOURCE_FAILURE_COVERAGE;
        failure->scope = failed->scope;
    } else if (root && root->discovery->nominated) {
        failure->kind = SH_PACKAGE_SOURCE_FAILURE_COVERAGE; failure->scope = failed->scope;
    } else failure->kind = SH_PACKAGE_SOURCE_FAILURE_PRODUCT;
}

/* Discover every unit from *inspected on, including those it requests. A
 * unit waiting for native preparation is left for a fresh pass; any other
 * failure stops the capture with that unit (sl_attribute). */
/* A failure its scope alone answers for (SOURCE or COVERAGE, sl_attribute):
 * keep the first, withdraw that scope from this capture and let the other
 * scopes continue, so the same attempt also meets every other scope's
 * failures and preparation requests. sl_phase_complete reports it. 0 when
 * the failure is not such (it stops the capture now). */
static int sl_withdrawal_add(sh_package_source_withdrawals *w, const sh_package_source_failure *failure,
    const char *reason)
{
    if (w->count == w->capacity) {
        size_t capacity = w->capacity ? w->capacity * 2 : 4;
        sh_package_source_withdrawal *grown = realloc(w->items, capacity * sizeof(*grown));
        if (!grown) return 0;
        w->items = grown; w->capacity = capacity;
    }
    sh_package_source_withdrawal *item = w->items + w->count++;
    item->failure = *failure;
    snprintf(item->reason, sizeof(item->reason), "%s", reason ? reason : "");
    return 1;
}
void sh_package_source_withdrawals_free(sh_package_source_withdrawals *withdrawals)
{
    if (!withdrawals) return;
    free(withdrawals->items);
    withdrawals->items = NULL; withdrawals->count = withdrawals->capacity = 0;
}
static int sl_defer_failure(sh_package_source_link *s)
{
    sh_package_source_failure failure;
    if (s->environment) return 0;
    sl_attribute(s, &failure);
    if ((failure.kind != SH_PACKAGE_SOURCE_FAILURE_SOURCE && failure.kind != SH_PACKAGE_SOURCE_FAILURE_COVERAGE) ||
        failure.scope >= s->scope_count) return 0;
    if (!s->withdrawn && !(s->withdrawn = calloc(s->scope_count, 1))) return 0;
    /* Every withdrawal is kept for the caller, not only the first: each
     * independent scope that fails on its own is reported by this one capture. */
    if (s->native.withdrawals && !sl_withdrawal_add(s->native.withdrawals, &failure,
            s->error && *s->error ? s->error : NULL)) return 0;
    if (!s->deferred.unit) {
        s->deferred.unit = s->failed_unit; s->deferred.malformed = s->malformed; s->deferred.reader_gap = s->reader_gap;
        snprintf(s->deferred.error, sizeof(s->deferred.error), "%s", s->error && *s->error ? s->error : "");
    }
    s->withdrawn[failure.scope] = 1;
    s->failed = 0; s->failed_unit = NULL;
    if (s->error && s->error_capacity) *s->error = 0;
    return 1;
}
/* End of a discovery phase: report a kept failure (as it was attributed), or
 * fail for the preparation requests retained so far. */
static int sl_phase_complete(sh_package_source_link *s)
{
    if (s->deferred.unit) {
        s->failed = 1; s->failed_unit = s->deferred.unit;
        s->malformed = s->deferred.malformed; s->reader_gap = s->deferred.reader_gap;
        if (s->error && s->error_capacity) snprintf(s->error, s->error_capacity, "%s", s->deferred.error);
        return 0;
    }
    if (*s->preparation_reason) return sl_env_fail(s, s->preparation_reason, NULL);
    return 1;
}
/* Discover every unit from *inspected on, including those it requests. A
 * unit waiting for native preparation is left for a fresh pass; a failure its
 * scope alone answers for withdraws that scope (sl_defer_failure); any other
 * failure stops the capture with that unit (sl_attribute), though a failure
 * kept earlier is reported before a later one that is not environmental. */
/* One unit of a discovery loop: 1 to go on, 0 when the capture stops. */
static int sl_discover_one(sh_package_source_link *s, sl_unit *unit)
{
    if (s->withdrawn && s->withdrawn[unit->scope]) return 1;
    s->reader_gap = s->malformed = s->preparing = 0;
    int discovered = sl_discover_unit(s, unit);
    s->census.units++;
    if (discovered) {
        if (unit->unit->file_count > 1) qsort((void *)unit->unit->files, unit->unit->file_count, sizeof(*unit->unit->files), sl_file_order);
        if (sl_movie_identities(s, unit)) return 1;
    } else if (s->preparing) return 1;
    s->failed_unit = unit;
    if (sl_defer_failure(s)) return 1;
    /* The stop depends on the order units were discovered in: with shared stock, scope by scope rather than the
     * capture's creation order, so the evidence gathered before it can differ (sh_package_source_withdrawals.stopped). */
    if (s->deferred.unit && !s->environment) {
        s->failed = 0; s->failed_unit = NULL;
        if (s->error && s->error_capacity) *s->error = 0;
        return 1;
    }
    if (!s->environment && s->native.withdrawals && s->share.scope_major) s->native.withdrawals->stopped = 1;
    return 0;
}
static int sl_discover_until(sh_package_source_link *s, size_t *inspected, size_t end)
{
    for (; *inspected < s->count && *inspected < end; (*inspected)++)
        if (!sl_discover_one(s, s->units[*inspected])) return 0;
    return 1;
}
static int sl_discover_through(sh_package_source_link *s, size_t *inspected)
{ return sl_discover_until(s, inspected, SIZE_MAX); }
/* A unit's own allocations besides its record and discovery lists (at close, and for a copy shared stock replaced). */
static void sl_unit_parts_free(sl_unit *unit)
{
    if (unit->discovery) {
        free(unit->discovery->seed); free(unit->discovery->implicit); free(unit->discovery->availability_semantics);
    }
    free((void *)unit->unit->files);
    for (size_t w = 0; w < unit->unit->witness_count; w++) free((char *)unit->unit->witnesses[w].role);
    free((void *)unit->unit->witnesses);
    free(unit->implicit_edges);
    sl_extra *extra = unit->extra;
    if (extra) {
        free(extra->original_note); free(extra->fsm_config);
        for (size_t p = 0; p < extra->program_count; p++) {
            free(extra->programs[p].name); sh_package_shader_program_clear(&extra->programs[p].decoded);
        }
        free(extra->programs); free(extra->shader_files);
        if (extra->identities) for (size_t j = 0; j < unit->unit->file_count; j++) free((char *)extra->identities[j].name);
        free(extra->identities);
        if (extra->prepared) free(extra->prepared->body);
        free(extra->prepared); free(extra);
    }
    free((void *)unit->discovered);
}

/* ---- Shared stock: folding a scope's proven stock copies (R-344 stage A) -------
 * After each discovery pass of a package scope whose root plan allows it, the units that pass discovered are
 * folded. A unit is bound once in the baseline scope when:
 * - certificate: the native semantics in its scope and in the baseline are one original certificate, each member file
 *   and witness is the captured original itself (status 1), and nothing scope-dependent (audio program, image
 *   availability, delivery owners, preparation, coverage, campaign boundary, product original) applies to it;
 * - inputs: its discovery read only captured originals, through paths the baseline does not supply either
 *   (sl_share_inputs), so the baseline would discover it alike, and a state-machine behavior selects among the
 *   catalog's declarations only;
 * - closure: every unit it reaches, through every dependency, site, implicit and conditional request (cycles
 *   included: sh_package_link_originals), is such a unit or already shared;
 * - structure: where the baseline already has the unit, that unit is the same (files, witnesses, sites, requests
 *   and dependencies, under the same mapping), else this unit becomes the baseline's (stock_only until a baseline
 *   unit reaches it: sl_promote), unless the native implicit adapter requested something for it (per scope).
 * Units a scope's later contributions select or edit by scope stay the scope's own (the library's boundary falls
 * back to unshared copies for anything else: package_library.c, pl_share_boundary). Their scope's units then bind
 * the baseline unit (its share map). */
static void sl_units_compact(sh_package_source_link *s)
{
    size_t kept = 0;
    for (size_t i = 0; i < s->count; i++) {
        sl_unit *unit = s->units[i];
        if (!unit) continue;
        unit->index = (uint32_t)kept; s->units[kept++] = unit;
    }
    s->count = kept;
}
/* A copy a fold replaced by the baseline's equal unit: its slot, row and allocations go, its discovery record and
 * lists return to their arenas (sl_arena_take reuses them). */
static void sl_unit_drop(sh_package_source_link *s, sl_unit *unit)
{
    sl_discovery *d = unit->discovery;
    sl_unit_slot_remove(s, unit);
    s->units[unit->index] = NULL;
    sl_unit_parts_free(unit);
    sl_arena_release(&s->dependency_arena, d->dependencies, d->dependency_capacity, sizeof(*d->dependencies));
    sl_arena_release(&s->site_arena, d->sites, d->site_capacity, sizeof(*d->sites));
    sl_arena_release(&s->discovery_arena, d, 1, sizeof(*d));
    free(unit); s->share.live--;
}
/* Mode 2: the representatives decided so far, by unit and by family and name (open addressing). */
static size_t sl_rep_at(const sl_rep *slots, size_t capacity, const sl_unit *unit)
{
    size_t at = (size_t)(((uint64_t)(uintptr_t)unit >> 4) * UINT64_C(11400714819323198485)) & (capacity - 1);
    while (slots[at].unit && slots[at].unit != unit) at = (at + 1) & (capacity - 1);
    return at;
}
static size_t sl_rep_name_at(const sl_rep *slots, size_t capacity, const char *family, const char *name)
{
    size_t at = (size_t)sl_unit_hash(SIZE_MAX, family, name) & (capacity - 1);
    while (slots[at].unit && (strcmp(slots[at].unit->unit->family, family) || strcmp(slots[at].unit->unit->name, name)))
        at = (at + 1) & (capacity - 1);
    return at;
}
static sl_unit *sl_rep_of(const sh_package_source_link *s, const sl_unit *unit)
{
    if (!s->share.rep_capacity) return NULL;
    const sl_rep *slot = s->share.reps + sl_rep_at(s->share.reps, s->share.rep_capacity, unit);
    return slot->unit ? slot->rep : NULL;
}
static int sl_rep_grow(sl_rep **slots, size_t *capacity, size_t count, int by_name)
{
    if (count + 1 <= *capacity / 2) return 1;
    size_t next = *capacity ? *capacity * 2 : 1024;
    sl_rep *grown = next <= SIZE_MAX / sizeof(*grown) && !SL_SHARE_REFUSED() ? calloc(next, sizeof(*grown)) : NULL;
    if (!grown) return 0;
    for (size_t i = 0; i < *capacity; i++) if ((*slots)[i].unit) {
        const sl_unit *unit = (*slots)[i].unit;
        size_t at = by_name ? sl_rep_name_at(grown, next, unit->unit->family, unit->unit->name) : sl_rep_at(grown, next, unit);
        grown[at] = (*slots)[i];
    }
    free(*slots); *slots = grown; *capacity = next; return 1;
}
/* Record that unit would be bound by rep (rep == unit: it would become the stock unit; NULL: no longer shared). */
static int sl_rep_set(sh_package_source_link *s, const sl_unit *unit, sl_unit *rep)
{
    if (!sl_rep_grow(&s->share.reps, &s->share.rep_capacity, s->share.rep_count, 0)) return 0;
    sl_rep *slot = s->share.reps + sl_rep_at(s->share.reps, s->share.rep_capacity, unit);
    if (!slot->unit) { slot->unit = unit; s->share.rep_count++; }
    slot->rep = rep;
    if (rep != unit) return 1;
    if (!sl_rep_grow(&s->share.rep_names, &s->share.rep_name_capacity, s->share.rep_name_count, 1)) return 0;
    slot = s->share.rep_names + sl_rep_name_at(s->share.rep_names, s->share.rep_name_capacity, unit->unit->family,
        unit->unit->name);
    if (!slot->unit) { slot->unit = unit; slot->rep = rep; s->share.rep_name_count++; }
    return 1;
}
/* The stock unit a unit of this family and name would be bound to: the baseline's, or in mode 2 the copy that would
 * have become it. */
static sl_unit *sl_stock_of(const sh_package_source_link *s, const char *family, const char *name)
{
    sl_unit *unit = sl_find_raw(s, s->share.baseline, family, name);
    if (unit || s->share.mode != 2 || !s->share.rep_name_capacity) return unit;
    const sl_rep *slot = s->share.rep_names + sl_rep_name_at(s->share.rep_names, s->share.rep_name_capacity, family, name);
    return slot->unit ? slot->rep : NULL;
}
/* A target outside the batch being folded that is already shared stock, hence certified. */
static int sl_share_certified(const sh_package_source_link *s, const sl_unit *unit)
{
    if (s->share.mode == 2) { const sl_unit *rep = sl_rep_of(s, unit); return rep != NULL; }
    return unit->scope == s->share.baseline && unit->shared_stock;
}
/* What a target maps to: inside the batch, the stock unit it would be bound to (map, NULL for itself); outside, its
 * representative in mode 2. */
static const sl_unit *sl_canon(const sh_package_source_link *s, const sl_unit *unit, sl_unit *const *map)
{
    if (unit->discovery && unit->discovery->fold_slot) {
        const sl_unit *to = map[unit->discovery->fold_slot - 1];
        return to ? to : unit;
    }
    if (s->share.mode == 2) { const sl_unit *rep = sl_rep_of(s, unit); if (rep) return rep; }
    return unit;
}
/* What a captured original declaration states for sharing, read once for every scope that reaches it: AI
 * registration lists (a conductor), and registration-key sites (sh_package_source_bind_keys). */
static int sl_key_site_seen(void *context, const char *slot, const char *value, size_t length, const char **replacement)
{ (void)slot; (void)value; (void)length; if (replacement) *replacement = NULL; ++*(size_t *)context; return 0; }
static int sl_conductor_text(const unsigned char *body, size_t length)
{
    for (size_t at = 0; at + 10 <= length; at++)
        if (!memcmp(body + at, "aiTypeList", 10) || (at + 12 <= length && !memcmp(body + at, "globalAIMods", 12))) return 1;
    return 0;
}
static int sl_key_declaration(const sl_unit *unit)
{
    const sh_package_link_unit *u = unit->unit;
    return u->file_count == 1 && strcmp(u->family, "renderprog") && !sh_decl_native_binding_family(u->family, NULL) &&
        (u->files[0]->type || !strncmp(u->files[0]->engine_path, "generated/decls/", 16));
}
/* 1 a conductor (an entity declaration stating AI registration lists), 0 not, -1 unreadable. */
static int sl_conductor(sh_package_source_link *s, const sl_unit *unit)
{
    size_t length = 0;
    if (strcmp(unit->unit->family, "entitydef") || unit->unit->file_count != 1) return 0;
    unsigned char *body = sh_package_compilation_read(unit->unit->compilation, unit->unit->files[0], SIZE_MAX, &length,
        NULL, 0);
    if (!body) return -1;
    int conductor = sl_conductor_text(body, length);
    free(body); return conductor;
}
static const sl_stock_fact *sl_stock_fact_of(sh_package_source_link *s, const sl_unit *unit)
{
    const sh_compiled_resource *file = unit->unit->file_count ? unit->unit->files[0] : NULL;
    static const sl_stock_fact none = {0};
    if (!file) return &none;
    if (s->share.fact_count + 1 > s->share.fact_capacity / 2) {
        size_t next = s->share.fact_capacity ? s->share.fact_capacity * 2 : 1024;
        sl_stock_fact *grown = next <= SIZE_MAX / sizeof(*grown) && !SL_SHARE_REFUSED() ? calloc(next, sizeof(*grown)) : NULL;
        if (!grown) return NULL;
        for (size_t i = 0; i < s->share.fact_capacity; i++) if (s->share.facts[i].file) {
            size_t at = (size_t)(((uint64_t)(uintptr_t)s->share.facts[i].file >> 4) * UINT64_C(11400714819323198485)) & (next - 1);
            while (grown[at].file) at = (at + 1) & (next - 1);
            grown[at] = s->share.facts[i];
        }
        free(s->share.facts); s->share.facts = grown; s->share.fact_capacity = next;
    }
    size_t mask = s->share.fact_capacity - 1;
    size_t at = (size_t)(((uint64_t)(uintptr_t)file >> 4) * UINT64_C(11400714819323198485)) & mask;
    while (s->share.facts[at].file) {
        const sl_stock_fact *fact = s->share.facts + at;
        if (fact->file == file && fact->family == unit->unit->family && fact->name == unit->unit->name) return fact;
        at = (at + 1) & mask;
    }
    sl_stock_fact fact = {file, unit->unit->family, unit->unit->name, 0, 0};
    unsigned long long began = sl_now_us();
    fact.conductor = sl_conductor(s, unit);
    if (fact.conductor >= 0 && sl_key_declaration(unit)) {
        size_t length = 0, sites = 0, output_length = 0;
        unsigned char *output = NULL;
        char error[512];
        unsigned char *body = sh_package_compilation_read(unit->unit->compilation, file, SIZE_MAX, &length, NULL, 0);
        int status = body ? sh_package_source_bind_keys(s->readers[unit->scope], unit->unit->family, unit->unit->name,
            body, length, sl_key_site_seen, &sites, &output, &output_length, NULL, error, sizeof(error)) : -1;
        free(body); free(output);
        fact.keys = status < 0 ? -1 : sites != 0;
        s->share.key_scans++;
    }
    s->share.keys_us += sl_now_us() - began;
    s->share.facts[at] = fact; s->share.fact_count++;
    return s->share.facts + at;
}
/* The units scope's later contributions select or edit by scope: they stay its own, with all that reaches them.
 * Conservative (the library's boundary decides the rest); tests turn it off to exercise that boundary. */
#if defined(SH_PACKAGE_LIBRARY_TESTING) || defined(SH_PACKAGE_SOURCE_LINK_TESTING) || \
    defined(SH_RESOURCE_GRAPH_TESTING) || defined(SH_PACKAGE_RUNTIME_TESTING)
static int g_sl_share_conservative = 1;
void sh_package_source_link_test_share_conservative(int enabled) { g_sl_share_conservative = enabled != 0; }
/* Mutation controls (tests): drop one shared-stock clause, which some oracle must then catch. */
static int g_sl_share_mutation;
void sh_package_source_link_test_share_mutation(int clause) { g_sl_share_mutation = clause; }
#define SL_MUTATED(clause) (g_sl_share_mutation == (clause))
#define SL_SHARE_CONSERVATIVE g_sl_share_conservative
/* The suite-wide modes the environment selects (sl_test_environment), restored by tests that vary them. */
static int g_sl_share_default = 1, g_sl_conservative_default = 1, g_sl_stream_default = 1, g_sl_mutation_default;
static void sl_test_environment(void)
{
    static volatile long read;
    if (_InterlockedCompareExchange(&read, 1, 0)) return;
    char *mode = NULL, *conservative = NULL, *stream = NULL, *mutation = NULL;
    size_t length = 0;
    if (_dupenv_s(&mode, &length, "SMP_SHARE_MODE")) mode = NULL;
    if (_dupenv_s(&conservative, &length, "SMP_SHARE_CONSERVATIVE")) conservative = NULL;
    if (_dupenv_s(&stream, &length, "SMP_STREAM")) stream = NULL;
    if (_dupenv_s(&mutation, &length, "SMP_SHARE_MUTATION")) mutation = NULL;
    if (mode && *mode >= '0' && *mode <= '2') g_sl_share_default = *mode - '0';
    if (conservative && *conservative) g_sl_conservative_default = *conservative != '0';
    if (stream && *stream) g_sl_stream_default = *stream != '0';
    /* A whole-suite mutation run (s1-s8; l1-l2 are the library's, pl_share_environment). */
    if (mutation && mutation[0] == 's' && mutation[1] >= '1' && mutation[1] <= '8') g_sl_mutation_default = mutation[1] - '0';
    free(mode); free(conservative); free(stream); free(mutation);
    g_sl_share = g_sl_share_default; g_sl_share_conservative = g_sl_conservative_default; g_sl_stream = g_sl_stream_default;
    g_sl_share_mutation = g_sl_mutation_default;
}
void sh_package_source_link_test_share_defaults(void)
{
    sl_test_environment();
    g_sl_share = g_sl_share_default; g_sl_share_conservative = g_sl_conservative_default; g_sl_stream = g_sl_stream_default;
    g_sl_share_mutation = g_sl_mutation_default;
}
/* Process totals of the captures that closed: captures, units decided, joined, became stock, privatized, streamed
 * originals and their reads again. */
static size_t g_sl_totals[7];
void sh_package_source_link_test_share_totals(size_t totals[7]) { memcpy(totals, g_sl_totals, sizeof(g_sl_totals)); }
#define SL_SHARE_TOTALS(s) (g_sl_totals[0]++, g_sl_totals[1] += (s)->share.decided, g_sl_totals[2] += (s)->share.folded, \
    g_sl_totals[3] += (s)->share.rehomed, g_sl_totals[4] += (s)->share.privatized, g_sl_totals[5] += (s)->share.released, \
    g_sl_totals[6] += (s)->share.rereads)
#else
#define SL_SHARE_CONSERVATIVE 1
#define SL_MUTATED(clause) 0
#endif
static int sl_share_family(const sh_package_source_link *s, const sl_unit *unit)
{
    const char *family = unit->unit->family;
    /* Capture paths of their own: shader programs and flags (native ABI, never recorded), fonts and cooked movies. */
    if (!strcmp(family, "renderprog") || !strcmp(family, "renderprogflag") || !strcmp(family, "font") ||
        sh_package_movie_owner(family, unit->unit->name)) return 0;
    if (!SL_SHARE_CONSERVATIVE) return 1;
    return strcmp(family, "snapmoduleinfo") && strcmp(family, "weapon") && !sh_package_editor_contribution_phase(family) &&
        !(!strcmp(family, "animweb") && sh_package_source_link_player_hands(s, unit->unit->name));
}
enum { SL_KEEP_NONE, SL_KEEP_LOCAL, SL_KEEP_INPUTS, SL_KEEP_FAMILY, SL_KEEP_KEYS };
/* The structural part of a batch unit's certificate (no callbacks): SL_KEEP_NONE when it may be shared. */
static int sl_fold_structure(sh_package_source_link *s, size_t scope, const sl_unit *unit)
{
    const sh_package_link_unit *record = unit->unit;
    size_t baseline = s->share.baseline;
    if (unit->coverage || unit->campaign_boundary || unit->presentation || unit->discovery->product ||
        unit->discovery->nominated || SL_EXTRA(unit)->prepared || SL_EXTRA(unit)->program_count ||
        SL_EXTRA(unit)->shader_file_count || SL_EXTRA(unit)->identities || record->file_identities) return SL_KEEP_LOCAL;
    if (!sl_share_family(s, unit)) return SL_KEEP_FAMILY;
    if (record->file_count && !unit->discovery->stock_inputs && !SL_MUTATED(2)) return SL_KEEP_INPUTS;
    for (size_t i = 0; i < record->file_count + record->witness_count; i++) {
        const sh_compiled_resource *file = i < record->file_count ? record->files[i] :
            record->witnesses[i - record->file_count].resource;
        const sl_original *original = file ? sl_original_peek(s, file->engine_path) : NULL;
        if (!original || &original->resource != file || original->status != 1) return SL_KEEP_LOCAL;
        if (sl_lookup_supplied(s, baseline, file->engine_path) && !SL_MUTATED(2)) return SL_KEEP_INPUTS;
    }
    if (!record->file_count) {
        /* An engine-registered or admitted name: what its selection consulted is supplied by neither scope. */
        const sh_resource_catalog_entry *const *rows = NULL;
        size_t count = sh_resource_catalog_find(s->catalog, record->family, record->name, &rows);
        if (record->original_binding != 1) return SL_KEEP_LOCAL;
        for (size_t i = 0; i < count; i++) if (*rows[i]->path) return SL_KEEP_INPUTS;
        if (!count) {
            char *path = sh_package_cooked_path(record->family, record->name);
            if (!path) path = sl_decl_path(record->family, record->name);
            int supplied = !path || sl_lookup_supplied(s, baseline, path) || sl_lookup_supplied(s, scope, path);
            free(path);
            if (supplied) return SL_KEEP_INPUTS;
        }
    }
    if (!strcmp(record->family, "aibehavior") && SL_EXTRA(unit)->fsm_config &&
        (s->share.fsm_inputs[scope] || s->share.fsm_inputs[baseline]) && !SL_MUTATED(7)) return SL_KEEP_INPUTS;
    return SL_KEEP_NONE;
}
/* Whether name's registered native image is an exclusion placeholder that state scope authored names may be isolated
 * from (availability_authored, R-389): 1 with the private copy's semantics, else 0. The baseline scope never asks. */
static int sl_authored_excluded(sh_package_source_link *s, size_t scope, const char *name, unsigned char semantics[32])
{
    char error[512] = "";
    if (!s->native.availability_authored || (s->share.mode && scope == s->share.baseline)) return 0;
    return s->native.availability_authored(s->native.context, scope, name, semantics, error, sizeof(error)) == 1;
}
/* The native part of the certificate, in the scope and in the baseline (the explicit extra step; metered): the
 * semantics both certify, written to semantics. SL_KEEP_NONE when it holds. known, when not NULL, is the stock unit
 * of this family and name a fold already certified: its certificate holds the baseline's answers (semantics, image
 * availability, owners, audio program), which no later discovery changes, so only the scope's are asked. */
static int sl_fold_certificate(sh_package_source_link *s, size_t scope, const sl_unit *unit, const sl_unit *known,
    unsigned char semantics[32])
{
    const sh_package_link_unit *record = unit->unit;
    size_t baseline = s->share.baseline;
    char error[512] = "";
    unsigned char stock[32];
    unsigned long long began = sl_now_us();
    int ok = 0;
    s->share.certified++;
    if (SL_SHARE_CONSERVATIVE) {
        const sl_stock_fact *fact = sl_stock_fact_of(s, unit);
        if (!fact) return -1;
        if (fact->conductor) return SL_KEEP_FAMILY;
        if (fact->keys) return SL_KEEP_KEYS;
    }
    if (!record->file_count) {
        /* Its semantics are its admission's; a registered name's do not depend on the scope. */
        const sh_resource_catalog_entry *const *rows = NULL;
        size_t count = sh_resource_catalog_find(s->catalog, record->family, record->name, &rows);
        int registered = 0;
        for (size_t i = 0; i < count; i++) registered |= !*rows[i]->path && rows[i]->archive < 2;
        ok = registered || (s->native.admit && s->native.admit(s->native.context, baseline, record->family, record->name,
            stock, error, sizeof(error)) == 1 && !memcmp(stock, record->semantics, 32));
        if (ok) memcpy(semantics, record->semantics, 32);
        goto done;
    }
    if (!s->native.semantics) goto done;
    if (!strcmp(record->family, "sound") && s->native.audio &&
        (sh_package_audio_program_affected(s->native.audio, scope, record->name) ||
         sh_package_audio_program_contains(s->native.audio, scope, record->name) ||
         (!known && (sh_package_audio_program_affected(s->native.audio, baseline, record->name) ||
                     sh_package_audio_program_contains(s->native.audio, baseline, record->name))))) goto done;
    if (s->native.availability && !strcmp(record->family, "image") &&
        (s->native.availability(s->native.context, scope, record->name, stock, error, sizeof(error)) != 0 ||
         (!known && s->native.availability(s->native.context, baseline, record->name, stock, error, sizeof(error)) != 0)))
        goto done;
    /* An image state this scope authored names keeps the scope's own unit when its native image is an exclusion
     * placeholder: the scope may serve a private copy of it (sl_availability). */
    if (!strcmp(record->family, "image") && unit->authored_request && sl_authored_excluded(s, scope, record->name, stock))
        goto done;
    if (s->native.semantics(s->native.context, scope, record->family, record->name, semantics, error, sizeof(error)) != 1)
        goto done;
    if (known) memcpy(stock, known->unit->semantics, 32);
    else if (s->native.semantics(s->native.context, baseline, record->family, record->name, stock, error,
        sizeof(error)) != 1) goto done;
    if (memcmp(stock, semantics, 32)) goto done;
    if (s->native.owners) {
        const sh_package_owners *owners = NULL, *gameplay = NULL;
        if (s->native.owners(s->native.context, scope, record->family, record->name, &owners, &gameplay, error,
            sizeof(error)) != 1 || owners || gameplay) goto done;
        if (!known && (s->native.owners(s->native.context, baseline, record->family, record->name, &owners, &gameplay,
            error, sizeof(error)) != 1 || owners || gameplay)) goto done;
    }
    if (known) s->share.certificates_reused++;
    ok = 1;
done:
    s->share.certify_us += sl_now_us() - began;
    return ok ? SL_KEEP_NONE : SL_KEEP_LOCAL;
}
/* Whether u has the structure of t (its would-be stock unit), each target under the batch's mapping. */
static int sl_fold_same(const sh_package_source_link *s, const sl_unit *u, const sl_unit *t, sl_unit *const *map)
{
    const sh_package_link_unit *a = u->unit, *b = t->unit;
    const sl_discovery *x = u->discovery, *y = t->discovery;
    if (!y || !y->discovered_done || a->file_count != b->file_count || a->witness_count != b->witness_count ||
        u->discovered_count != t->discovered_count || u->site_count != t->site_count ||
        x->dependency_count != y->dependency_count || x->implicit_count != y->implicit_count ||
        u->coverage != t->coverage || u->campaign_boundary != t->campaign_boundary ||
        x->material_members != y->material_members || x->product != y->product) return 0;
    if (!a->file_count && (a->original_binding != b->original_binding || memcmp(a->semantics, b->semantics, 32))) return 0;
    for (size_t i = 0; i < a->file_count; i++) if (a->files[i] != b->files[i]) return 0;
    for (size_t i = 0; i < a->witness_count; i++)
        if (a->witnesses[i].resource != b->witnesses[i].resource || strcmp(a->witnesses[i].role, b->witnesses[i].role)) return 0;
    for (size_t i = 0; i < u->discovered_count; i++) if (u->discovered[i] != t->discovered[i]) return 0;
    for (size_t i = 0; i < u->site_count; i++) {
        const sl_site *p = x->sites + i, *q = y->sites + i;
        if (p->file != q->file || (p->slot != q->slot && strcmp(p->slot, q->slot)) ||
            sl_canon(s, p->target, map) != sl_canon(s, q->target, map)) return 0;
    }
    for (size_t i = 0; i < x->dependency_count; i++)
        if (sl_canon(s, x->dependencies[i], map) != sl_canon(s, y->dependencies[i], map)) return 0;
    for (size_t i = 0; i < x->implicit_count; i++) {
        const sl_implicit *p = x->implicit + i, *q = y->implicit + i;
        if (p->conditional != q->conditional || (p->slot != q->slot && strcmp(p->slot, q->slot)) ||
            sl_canon(s, p->target, map) != sl_canon(s, q->target, map)) return 0;
    }
    const char *c = SL_EXTRA(u)->fsm_config, *d = SL_EXTRA(t)->fsm_config;
    return (!c && !d) || (c && d && !strcmp(c, d));
}
/* A copy of a shared unit as scope's own (sl_privatize): its files, witnesses, requests and lists, with the same
 * targets until the caller maps them. NULL when it cannot be allocated. */
static sl_unit *sl_unit_clone(sh_package_source_link *s, const sl_unit *from, size_t scope)
{
    const sl_discovery *source = from->discovery;
    sl_unit *unit = NULL;
    uint32_t room = 0;
    if (s->count >= UINT32_MAX - 1 || !sl_grow((void ***)&s->units, &s->capacity, s->count) ||
        (s->count >= s->unit_slot_count / 2 && !sl_unit_rehash(s)) || SL_SHARE_REFUSED() ||
        !(unit = calloc(1, sizeof(*unit)))) return NULL;
    if (!(unit->discovery = sl_arena_take(&s->discovery_arena, 1, sizeof(*unit->discovery), &room))) { free(unit); return NULL; }
    sl_discovery *d = unit->discovery;
    *d = *source;
    unit->unit = &d->record;
    d->dependencies = NULL; d->dependency_capacity = 0; d->sites = NULL; d->site_capacity = 0; d->implicit = NULL;
    d->implicit_count = 0; d->dependency_count = 0; d->parent = NULL; d->seed = NULL; d->availability_semantics = NULL;
    d->fold_slot = 0; d->seeded = 0; d->nominated = 0; d->availability = 0; d->capacity = 0;
    d->record.files = NULL; d->record.file_count = 0; d->record.witnesses = NULL; d->record.witness_count = 0;
    d->record.compilation = s->scopes[scope];
    unit->scope = (uint32_t)scope; unit->index = (uint32_t)s->count;
    unit->replayed = from->replayed; unit->coverage = from->coverage;
    s->units[s->count++] = unit; sl_unit_slot_insert(s, unit);
    if (++s->share.live > s->share.peak_live) s->share.peak_live = s->share.live;
    /* From here a failure leaves a unit close frees. */
    const sh_package_link_unit *record = from->unit;
    if (record->file_count) {
        if (!(d->record.files = malloc(record->file_count * sizeof(*record->files)))) return NULL;
        memcpy((void *)d->record.files, record->files, record->file_count * sizeof(*record->files));
        d->record.file_count = record->file_count; d->capacity = (uint32_t)record->file_count;
    }
    if (record->witness_count) {
        sh_package_link_witness *witnesses = calloc(record->witness_count, sizeof(*witnesses));
        if (!witnesses) return NULL;
        d->record.witnesses = witnesses;
        for (size_t i = 0; i < record->witness_count; i++) {
            if (!(witnesses[i].role = _strdup(record->witnesses[i].role))) return NULL;
            witnesses[i].resource = record->witnesses[i].resource; d->record.witness_count = i + 1;
        }
    }
    if (source->dependency_count) {
        if (!(d->dependencies = sl_arena_take(&s->dependency_arena, source->dependency_count, sizeof(*d->dependencies),
            &d->dependency_capacity))) return NULL;
        memcpy(d->dependencies, source->dependencies, source->dependency_count * sizeof(*d->dependencies));
        d->dependency_count = source->dependency_count;
    }
    if (from->site_count) {
        if (!(d->sites = sl_arena_take(&s->site_arena, from->site_count, sizeof(*d->sites), &d->site_capacity))) return NULL;
        memcpy(d->sites, source->sites, from->site_count * sizeof(*d->sites));
        unit->site_count = from->site_count;
    }
    if (source->implicit_count) {
        if (!(d->implicit = malloc(source->implicit_count * sizeof(*d->implicit)))) return NULL;
        memcpy(d->implicit, source->implicit, source->implicit_count * sizeof(*d->implicit));
        d->implicit_count = source->implicit_count;
    }
    if (from->discovered_count) {
        if (!(unit->discovered = malloc(from->discovered_count * sizeof(*unit->discovered)))) return NULL;
        memcpy((void *)unit->discovered, from->discovered, from->discovered_count * sizeof(*unit->discovered));
        unit->discovered_count = from->discovered_count;
    }
    if (from->extra) {
        const sl_extra *extra = from->extra;
        /* Shared units have no shader programs, movie identities or preparation (sl_fold_structure). */
        if (extra->program_count || extra->shader_file_count || extra->identities || extra->prepared ||
            !(unit->extra = calloc(1, sizeof(*unit->extra)))) return NULL;
        if ((extra->fsm_config && !(unit->extra->fsm_config = _strdup(extra->fsm_config))) ||
            (extra->original_note && !(unit->extra->original_note = _strdup(extra->original_note)))) return NULL;
    }
    return unit;
}
/* A later trigger in scope (a conductor naming a shared entity, or the test hook) needs its own copies of shared
 * units: each gets back the scope's own unit, so does every shared unit the scope binds that reaches one (cycles
 * included), and the scope's units bind those copies; the shared units stay for the other scopes. Mode 2 stops
 * recording the same units as shared. 0 on allocation failure (an environment failure). */
static int sl_privatize(sh_package_source_link *s, size_t scope, sl_unit **triggers, size_t count)
{
    if (!count) return 1;
    if (s->share.mode == 2) {
        /* The scope's units recorded as shared that reach a trigger, through others such, stop being shared. */
        size_t marked = 0;
        for (size_t t = 0; t < count; t++) if (sl_rep_of(s, triggers[t]) && !triggers[t]->discovery->fold_slot) {
            if (!sl_rep_set(s, triggers[t], NULL)) return sl_env_fail(s, "cannot record shared stock", NULL);
            triggers[t]->discovery->fold_slot = 1; marked++;
        }
        for (int again = marked != 0 && !SL_MUTATED(8); again;) {
            again = 0;
            for (size_t i = 0; i < s->count; i++) {
                sl_unit *unit = s->units[i];
                if (!unit || unit->scope != scope || unit->discovery->fold_slot || !sl_rep_of(s, unit)) continue;
                for (uint32_t d = 0; d < unit->discovery->dependency_count; d++) {
                    if (!unit->discovery->dependencies[d]->discovery->fold_slot) continue;
                    if (!sl_rep_set(s, unit, NULL)) return sl_env_fail(s, "cannot record shared stock", NULL);
                    unit->discovery->fold_slot = 1; marked++; again = 1; break;
                }
            }
        }
        for (size_t i = 0; i < s->count; i++) if (s->units[i] && s->units[i]->scope == scope) s->units[i]->discovery->fold_slot = 0;
        s->share.privatized += marked; s->share.privatizations++;
        return 1;
    }
    sl_share_map *map = s->share.maps + scope;
    size_t m = map->count, head = 0, tail = 0, edges = 0;
    if (!m) return 1;
    unsigned char *in = calloc(m, 1);
    sl_unit **clones = calloc(m, sizeof(*clones));
    size_t *queue = malloc(m * sizeof(*queue)), *offsets = calloc(m + 1, sizeof(*offsets)), *cursor = calloc(m, sizeof(*cursor));
    size_t *incoming = NULL;
    int ok = 0;
    if (!in || !clones || !queue || !offsets || !cursor || SL_SHARE_REFUSED()) goto done;
    for (size_t p = 0; p < m; p++) map->units[p]->discovery->fold_slot = (uint32_t)(p + 1);
    /* Reverse edges among the units the scope shares: its map is closed under their edges. */
    for (size_t p = 0; p < m; p++) {
        const sl_discovery *d = map->units[p]->discovery;
        for (uint32_t e = 0; e < d->dependency_count; e++) {
            uint32_t slot = d->dependencies[e]->discovery->fold_slot;
            if (slot) { offsets[slot]++; edges++; }
        }
    }
    for (size_t p = 0; p < m; p++) { offsets[p + 1] += offsets[p]; cursor[p] = offsets[p]; }
    if (!(incoming = malloc((edges ? edges : 1) * sizeof(*incoming)))) goto marked;
    for (size_t p = 0; p < m; p++) {
        const sl_discovery *d = map->units[p]->discovery;
        for (uint32_t e = 0; e < d->dependency_count; e++) {
            uint32_t slot = d->dependencies[e]->discovery->fold_slot;
            if (slot) incoming[cursor[slot - 1]++] = p;
        }
    }
    for (size_t t = 0; t < count; t++) {
        uint32_t slot = triggers[t]->scope == s->share.baseline ? triggers[t]->discovery->fold_slot : 0;
        if (slot && !in[slot - 1]) { in[slot - 1] = 1; queue[tail++] = slot - 1; }
    }
    while (head < tail && !SL_MUTATED(8)) {
        size_t p = queue[head++];
        for (size_t k = offsets[p]; k < offsets[p + 1]; k++)
            if (!in[incoming[k]]) { in[incoming[k]] = 1; queue[tail++] = incoming[k]; }
    }
    for (size_t p = 0; p < m; p++) if (in[p] && !(clones[p] = sl_unit_clone(s, map->units[p], scope))) goto marked;
    /* The copies and the scope's own units bind the copies instead of the shared units. */
    for (size_t i = 0; i < s->count; i++) {
        sl_unit *unit = s->units[i];
        if (!unit || unit->scope != scope || !unit->discovery) continue;
        sl_discovery *d = unit->discovery;
#define SL_PRIVATE(target) ((target)->scope == s->share.baseline && (target)->discovery->fold_slot && \
    in[(target)->discovery->fold_slot - 1] ? clones[(target)->discovery->fold_slot - 1] : (target))
        for (uint32_t e = 0; e < d->dependency_count; e++) d->dependencies[e] = SL_PRIVATE(d->dependencies[e]);
        for (uint32_t e = 0; e < unit->site_count; e++) d->sites[e].target = SL_PRIVATE(d->sites[e].target);
        for (uint32_t e = 0; e < d->implicit_count; e++) d->implicit[e].target = SL_PRIVATE(d->implicit[e].target);
#undef SL_PRIVATE
    }
    ok = 1;
marked:
    for (size_t p = 0; p < m; p++) map->units[p]->discovery->fold_slot = 0;
    if (ok) {
        size_t kept = 0;
        for (size_t p = 0; p < m; p++) {
            if (in[p]) { s->share.privatized++; continue; }
            map->units[kept++] = map->units[p];
        }
        map->count = map->sorted = kept;
        s->share.privatizations++;
    }
done:
    free(in); free(clones); free(queue); free(offsets); free(cursor); free(incoming);
    return ok ? 1 : sl_env_fail(s, "cannot give a scope its own copies of shared stock", NULL);
}
enum { SL_KEEP_EQUAL = SL_KEEP_KEYS + 1 };
/* Fold the units scope discovered in its last pass (s->share.batch). 0 only for an environment failure. */
static int sl_fold_scope(sh_package_source_link *s, size_t scope)
{
    size_t n = s->share.batch_count, edge_count = 0, at = 0, trigger_count = 0, trigger_capacity = 0;
    sl_unit **batch = s->share.batch, **map = NULL, **triggers = NULL;
    unsigned char *local = NULL, *certified = NULL, *checked = NULL, (*semantics)[32] = NULL;
    signed char *conductor = NULL;
    sh_package_link_node *nodes = NULL;
    sh_package_link_edge *edges = NULL;
    int ok = 0, *reason = NULL, slots = 0;
    unsigned long long began = sl_now_us();
    s->share.batch_count = 0;
    if (!sl_share_scope(s, scope) || scope == s->share.baseline || (s->withdrawn && s->withdrawn[scope]) ||
        *s->preparation_reason || s->deferred.unit || s->failed) return 1;
    s->share.folds++;
    if (!(conductor = calloc(n + 1, 1))) goto failed;
    /* A conductor (an entity stating AI registration lists) keeps the entities it names its scope's own; those this
     * scope already shares get the scope's own copies first (sl_privatize), as does the test hook's unit. */
    for (size_t i = 0; i < n; i++) {
        conductor[i] = (signed char)sl_conductor(s, batch[i]);
        if (!conductor[i]) continue;
        const sl_discovery *d = batch[i]->discovery;
        s->share.conductors++;
        for (uint32_t e = 0; e < d->dependency_count; e++) {
            sl_unit *target = d->dependencies[e];
            if (strcmp(target->unit->family, "entitydef") || !sl_share_certified(s, target)) continue;
            if (!sl_units_push(&triggers, &trigger_count, &trigger_capacity, target)) goto failed;
        }
    }
#if defined(SH_PACKAGE_LIBRARY_TESTING) || defined(SH_PACKAGE_SOURCE_LINK_TESTING)
    if (g_sl_privatize_scope == scope) {
        sl_unit *shared = s->share.mode == 2 ? sl_find_raw(s, scope, g_sl_privatize_family, g_sl_privatize_name) :
            sl_share_find(s, scope, g_sl_privatize_family, g_sl_privatize_name);
        if (shared && s->share.mode == 2 && !sl_rep_of(s, shared)) shared = NULL;
        if (shared) {
            g_sl_privatize_scope = SIZE_MAX;
            if (!sl_units_push(&triggers, &trigger_count, &trigger_capacity, shared)) goto failed;
        }
    }
#endif
    if (trigger_count && !sl_privatize(s, scope, triggers, trigger_count)) goto done;
    if (!n) { ok = 1; goto done; }
    local = calloc(n + 1, 1); certified = calloc(n + 1, 1); checked = calloc(n + 1, 1);
    semantics = calloc(n + 1, sizeof(*semantics)); map = calloc(n + 1, sizeof(*map));
    nodes = calloc(n + 1, sizeof(*nodes)); reason = calloc(n + 1, sizeof(*reason));
    if (!local || !certified || !checked || !semantics || !map || !nodes || !reason || SL_SHARE_REFUSED()) goto failed;
    for (size_t i = 0; i < n; i++) {
        if (batch[i]->discovery->dependency_count > SIZE_MAX - edge_count) goto failed;
        edge_count += batch[i]->discovery->dependency_count;
    }
    if (!(edges = calloc(edge_count ? edge_count : 1, sizeof(*edges)))) goto failed;
    for (size_t i = 0; i < n; i++) batch[i]->discovery->fold_slot = (uint32_t)(i + 1);
    slots = 1;
    /* Every site and every implicit and conditional request is also a dependency (sl_reference, sl_implicit_add).
     * A shared target is certified; any other target outside the batch (this scope's own unit of an earlier pass, or
     * one left undiscovered) is not: the sink, node n. */
    for (size_t i = 0; i < n; i++) {
        const sl_discovery *d = batch[i]->discovery;
        size_t first = at;
        for (uint32_t e = 0; e < d->dependency_count; e++) {
            const sl_unit *target = d->dependencies[e];
            if (target->discovery && target->discovery->fold_slot) edges[at++].target = target->discovery->fold_slot - 1;
            else if (!sl_share_certified(s, target)) edges[at++].target = n;
        }
        nodes[i].edges = edges + first; nodes[i].edge_count = at - first;
        reason[i] = sl_fold_structure(s, scope, batch[i]);
        local[i] = reason[i] == SL_KEEP_NONE;
    }
    /* A conductor, and the entities it names, stay their scope's own. */
    for (size_t i = 0; i < n; i++) if (conductor[i] && !SL_MUTATED(6)) {
        local[i] = 0; reason[i] = conductor[i] < 0 ? SL_KEEP_LOCAL : SL_KEEP_FAMILY;
        const sl_discovery *d = batch[i]->discovery;
        for (uint32_t e = 0; e < d->dependency_count; e++) {
            const sl_unit *target = d->dependencies[e];
            if (!target->discovery || !target->discovery->fold_slot || strcmp(target->unit->family, "entitydef")) continue;
            local[target->discovery->fold_slot - 1] = 0; reason[target->discovery->fold_slot - 1] = SL_KEEP_FAMILY;
        }
    }
    /* The closure, then the certificate of what it keeps, then the structure each would join: each removal can
     * remove what reaches it, so the walk repeats until nothing changes. */
    for (;;) {
        int changed = 0;
        if (!sh_package_link_originals(nodes, n + 1, local, certified, NULL, 0)) goto failed;
        if (SL_MUTATED(1)) memcpy(certified, local, n + 1); /* mutation: no closure */
        for (size_t i = 0; i < n; i++) if (certified[i] && !checked[i]) {
            /* A stock unit an earlier fold certified answers for the baseline (deciding only keeps no such unit). */
            const sl_unit *known = s->share.mode == 1 ?
                sl_find_raw(s, s->share.baseline, batch[i]->unit->family, batch[i]->unit->name) : NULL;
            checked[i] = 1;
            int keep = SL_MUTATED(4) ? SL_KEEP_NONE :
                sl_fold_certificate(s, scope, batch[i], known && known->fold_certified ? known : NULL, semantics[i]);
            if (keep < 0) goto failed;
            if (keep != SL_KEEP_NONE) { local[i] = 0; reason[i] = keep; changed = 1; }
        }
        if (changed) continue;
        for (size_t i = 0; i < n; i++)
            map[i] = certified[i] ? sl_stock_of(s, batch[i]->unit->family, batch[i]->unit->name) : NULL;
        /* A unit no baseline unit has yet becomes the baseline's only where the baseline would discover it alike: not
         * one the native implicit adapter, which may answer per scope, requested anything for (it may still join). */
        for (size_t i = 0; i < n; i++)
            if (certified[i] && !map[i] && batch[i]->discovery->native_requests && !SL_MUTATED(2)) {
                local[i] = 0; reason[i] = SL_KEEP_INPUTS; changed = 1;
            }
        for (size_t i = 0; i < n; i++) if (certified[i] && map[i]) {
            const sl_unit *stock = map[i];
            int same = SL_MUTATED(3) || sl_fold_same(s, batch[i], stock, map);
            /* A stock unit certified at an earlier fold carries that certificate's semantics. */
            if (same && stock->fold_certified && memcmp(stock->unit->semantics, semantics[i], 32)) same = 0;
            if (!same) { local[i] = 0; reason[i] = SL_KEEP_EQUAL; changed = 1; }
        }
        if (!changed) break;
    }
    for (size_t i = 0; i < n; i++) {
        sl_unit *unit = batch[i];
        if (!certified[i]) {
            s->share.survivors++;
            if (SL_REUSE_DEBUG() && strncmp(unit->unit->family, "entitydef", 9)) fprintf(stderr, "fold: scope %zu pass %u keeps %s %s (reason %d)\n",
                scope, s->reuse.pass, unit->unit->family, unit->unit->name, reason[i]);
            switch (reason[i]) {
            case SL_KEEP_NONE: s->share.blocked_closure++; break;
            case SL_KEEP_INPUTS: s->share.blocked_inputs++; break;
            case SL_KEEP_FAMILY: s->share.blocked_family++; break;
            case SL_KEEP_KEYS: s->share.blocked_keys++; break;
            case SL_KEEP_EQUAL: s->share.blocked_equal++; break;
            default: s->share.blocked_local++; break;
            }
            continue;
        }
        s->share.decided++;
        if (SL_REUSE_DEBUG()) fprintf(stderr, "fold: scope %zu pass %u shares %s %s (%s)\n", scope, s->reuse.pass,
            unit->unit->family, unit->unit->name, map[i] ? "joins" : "becomes stock");
        if (s->share.mode == 2) {
            if (!sl_rep_set(s, unit, map[i] ? map[i] : unit)) goto failed;
            continue;
        }
        sl_unit *stock = map[i] ? map[i] : unit;
        if (stock == unit) {
            /* It becomes the baseline's unit: certified here, a member of the baseline scope once a baseline unit
             * reaches it (sl_promote). Its first requester and seeding were this scope's. */
            sl_unit_slot_remove(s, unit);
            unit->scope = (uint32_t)s->share.baseline; unit->unit->compilation = s->scopes[s->share.baseline];
            unit->stock_only = 1; unit->authored_request = unit->authored_layer = 0;
            unit->discovery->seeded = 0; unit->discovery->parent = NULL;
            free(unit->discovery->seed); unit->discovery->seed = NULL;
            sl_unit_slot_insert(s, unit);
            s->share.rehomed++;
        } else s->share.folded++;
        if (!stock->fold_certified) {
            memcpy(stock->unit->semantics, semantics[i], 32); stock->unit->original_binding = 1; stock->fold_certified = 1;
        }
        if (!stock->shared_stock) s->share.stock_units++;
        stock->shared_stock = 1;
        sl_share_map *shares = s->share.maps + scope;
        if (!sl_units_push(&shares->units, &shares->count, &shares->capacity, stock)) goto failed;
        if (s->reuse.out && !sl_reuse_mark(s, scope, stock, unit->discovery->consulted, NULL)) goto done;
    }
    if (s->share.mode == 1) {
        /* The scope's remaining units, its seeds and the moved ones bind the baseline units the copies became or
         * joined. Units of the scope's earlier passes were complete before these existed. */
        const sl_pending *seeds = s->share.pending + scope;
        for (size_t i = 0; i < n + seeds->admitted_count; i++) {
            sl_unit *unit = i < n ? batch[i] : seeds->admitted[i - n];
            if (i < n && certified[i] && map[i] && map[i] != unit) continue; /* dropped below */
            sl_discovery *d = unit->discovery;
            for (uint32_t e = 0; e < d->dependency_count; e++) d->dependencies[e] = (sl_unit *)sl_canon(s, d->dependencies[e], map);
            for (uint32_t e = 0; e < unit->site_count; e++) d->sites[e].target = (sl_unit *)sl_canon(s, d->sites[e].target, map);
            for (uint32_t e = 0; e < d->implicit_count; e++) d->implicit[e].target = (sl_unit *)sl_canon(s, d->implicit[e].target, map);
        }
        for (size_t i = 0; i < n; i++) batch[i]->discovery->fold_slot = 0;
        slots = 0;
        for (size_t i = 0; i < n; i++) if (certified[i] && map[i] && map[i] != batch[i]) sl_unit_drop(s, batch[i]);
        sl_share_map *shares = s->share.maps + scope;
        if (shares->count > 1) qsort(shares->units, shares->count, sizeof(*shares->units), sl_share_order);
        shares->sorted = shares->count;
    }
    ok = 1;
    goto done;
failed:
    sl_env_fail(s, "cannot fold shared stock", NULL);
done:
    if (slots) for (size_t i = 0; i < n; i++) batch[i]->discovery->fold_slot = 0;
    free(local); free(certified); free(checked); free(semantics); free(map); free(nodes); free(edges); free(reason);
    free(triggers); free(conductor);
    s->share.fold_us += sl_now_us() - began;
    return ok;
}
/* ---- Unchanged-scope reuse (R-348) -------------------------------------------------------------------------
 * A package scope whose compiled content, attributes and capture context are unchanged (sl_reuse_scope_key) reaches
 * what it reached before, given the same baseline. What its folds bound as shared stock is recorded with the pass
 * that bound it (sl_reuse_mark, sl_reuse_build), and a later capture offers those bindings to the scope with an equal
 * key, pass by pass, before it discovers that pass (sl_reuse_begin). A binding is valid when:
 * - the scope has no unit of its own of that family and name (in the first pass, an admission copy not discovered
 *   yet is replaced by the stock unit, as the fold would replace it);
 * - the baseline has a discovered stock unit of that family and name whose fold structure has the recorded digest
 *   (sl_reuse_digest_of: files, witnesses and discovered files by path and resolution, sites, dependencies and
 *   requests by target name and scope, state-machine configuration, flags, a file-less unit's semantics), with none
 *   of the structure sl_fold_structure keeps local, of a family that shares, not a conductor;
 * - neither the baseline nor the scope supplies one of its files or a path the scope's copy consulted;
 * - its certificate holds for the scope now: sl_fold_certificate, asked exactly as the fold asks it for the scope's
 *   copy (the stock unit is known when an earlier fold or binding certified it);
 * - every unit it depends on is bound by the scope already or valid too (cycles included).
 * A valid binding is bound when the scope first requests its family and name (sl_reuse_bind), with its closure, as
 * the fold binds an equal copy: the stock unit is certified with the scope's certificate when nothing had certified
 * it yet, and joins the scope's map. Everything else the scope reaches is discovered and folded as usual, so a
 * refused binding costs only the reuse; a scope never promotes a unit from its record. A restore that cannot
 * complete (an allocation, a contradiction between record and capture) fails the capture as an environment failure;
 * the library then compiles without reuse. */
struct sh_package_source_reuse {
    char **strings; size_t string_count, string_capacity; /* open addressing; owned */
    const char **paths; size_t path_count, path_capacity;
    sl_reuse_stock *stock; size_t stock_count, stock_capacity;
    sl_reuse_binding *bindings; size_t binding_count, binding_capacity;
    sl_reuse_scope *scopes; size_t scope_count, scope_capacity;
    /* Settled groups (sh_package_source_reuse_add_settled): sorted by key and unique once settled_sorted ==
     * settled_count. */
    struct sr_settled *settled; size_t settled_count, settled_capacity, settled_sorted;
    /* Identity templates (sh_package_source_reuse_set_templates): their bytes, owned; or, loaded, the sealed file
     * holding them (retained) and where. */
    unsigned char *templates; size_t template_length;
    sh_package_file *template_file; uint64_t template_offset, template_file_length;
};
/* A settled editor group's key and what it staged, as the library encoded it (none: nothing staged); owned. */
typedef struct sr_settled { unsigned char key[32]; unsigned char *outputs; size_t length; } sr_settled;
sh_package_source_reuse *sh_package_source_reuse_open(void) { return calloc(1, sizeof(sh_package_source_reuse)); }
size_t sh_package_source_reuse_bytes(const sh_package_source_reuse *r)
{
    if (!r) return 0;
    size_t bytes = sizeof(*r) + r->string_capacity * sizeof(*r->strings) + r->path_capacity * sizeof(*r->paths) +
        r->stock_capacity * sizeof(*r->stock) + r->binding_capacity * sizeof(*r->bindings) +
        r->scope_capacity * sizeof(*r->scopes) + r->settled_capacity * sizeof(*r->settled);
    for (size_t i = 0; i < r->string_capacity; i++) if (r->strings[i]) bytes += strlen(r->strings[i]) + 1;
    for (size_t i = 0; i < r->settled_count; i++) bytes += r->settled[i].length;
    return bytes + r->template_length;
}
static void sr_clear(sh_package_source_reuse *r)
{
    for (size_t i = 0; i < r->string_capacity; i++) free(r->strings[i]);
    for (size_t i = 0; i < r->settled_count; i++) free(r->settled[i].outputs);
    free(r->strings); free(r->paths); free(r->stock); free(r->bindings); free(r->scopes); free(r->settled);
    free(r->templates); sh_package_file_release(r->template_file);
    memset(r, 0, sizeof(*r));
}
void sh_package_source_reuse_set_templates(sh_package_source_reuse *r, unsigned char *bytes, size_t length)
{
    if (!r) { free(bytes); return; }
    free(r->templates); r->templates = bytes; r->template_length = bytes ? length : 0;
}
const unsigned char *sh_package_source_reuse_templates(const sh_package_source_reuse *r, size_t *length)
{
    if (length) *length = r && r->templates ? r->template_length : 0;
    return r ? r->templates : NULL;
}
void sh_package_source_reuse_attach_templates(sh_package_source_reuse *r, sh_package_file *file, uint64_t offset,
    uint64_t length)
{
    if (!r) return;
    sh_package_file_release(r->template_file);
    r->template_file = file && length ? sh_package_file_retain(file) : NULL;
    r->template_offset = r->template_file ? offset : 0; r->template_file_length = r->template_file ? length : 0;
}
int sh_package_source_reuse_read_templates(const sh_package_source_reuse *r, unsigned char **bytes, size_t *length)
{
    *bytes = NULL; *length = 0;
    if (!r || (!r->template_file && !r->templates)) return 0;
    if (!r->template_file) {
        if (!(*bytes = malloc(r->template_length ? r->template_length : 1))) return -1;
        memcpy(*bytes, r->templates, r->template_length); *length = r->template_length;
        return 1;
    }
    uint64_t total = 0;
    if (r->template_file_length > SIZE_MAX) return -1;
    FILE *stream = sh_package_file_open(r->template_file, &total, NULL, 0);
    unsigned char *out = stream ? malloc((size_t)r->template_file_length) : NULL;
    int ok = out && r->template_offset <= total && r->template_file_length <= total - r->template_offset &&
        r->template_offset <= INT64_MAX && !_fseeki64(stream, (long long)r->template_offset, SEEK_SET) &&
        fread(out, 1, (size_t)r->template_file_length, stream) == r->template_file_length;
    if (stream) fclose(stream);
    if (!ok) { free(out); return -1; }
    *bytes = out; *length = (size_t)r->template_file_length;
    return 1;
}
void sh_package_source_reuse_close(sh_package_source_reuse *reuse) { if (reuse) { sr_clear(reuse); free(reuse); } }
void sh_package_source_reuse_move(sh_package_source_reuse *dst, sh_package_source_reuse *src)
{
    if (!dst || !src || dst == src) return;
    sr_clear(dst); *dst = *src; memset(src, 0, sizeof(*src));
}
size_t sh_package_source_reuse_scope_count(const sh_package_source_reuse *reuse) { return reuse ? reuse->scope_count : 0; }
static int sr_settled_order(const void *a, const void *b)
{ return memcmp(((const sr_settled *)a)->key, ((const sr_settled *)b)->key, 32); }
static int sr_settled_pointer_order(const void *a, const void *b)
{ return sr_settled_order(*(const sr_settled *const *)a, *(const sr_settled *const *)b); }
int sh_package_source_reuse_add_settled(sh_package_source_reuse *r, const unsigned char key[32],
    const unsigned char *outputs, size_t length)
{
    if (!r || !key || (length && !outputs)) return 0;
    if (r->settled_count == r->settled_capacity) {
        size_t next = r->settled_capacity ? r->settled_capacity + r->settled_capacity / 2 : 256;
        sr_settled *grown = next <= SIZE_MAX / sizeof(*grown) ? realloc(r->settled, next * sizeof(*grown)) : NULL;
        if (!grown) return 0;
        r->settled = grown; r->settled_capacity = next;
    }
    sr_settled *entry = r->settled + r->settled_count;
    memcpy(entry->key, key, 32); entry->outputs = NULL; entry->length = length;
    if (length && !(entry->outputs = malloc(length))) return 0;
    if (length) memcpy(entry->outputs, outputs, length);
    r->settled_count++;
    return 1;
}
int sh_package_source_reuse_settled(const sh_package_source_reuse *r, const unsigned char key[32],
    const unsigned char **outputs, size_t *length)
{
    const sr_settled *found = NULL;
    if (outputs) *outputs = NULL;
    if (length) *length = 0;
    if (!r || !key || !r->settled_count) return 0;
    sr_settled probe; memcpy(probe.key, key, 32);
    if (r->settled_sorted == r->settled_count)
        found = bsearch(&probe, r->settled, r->settled_count, sizeof(*r->settled), sr_settled_order);
    else for (size_t i = 0; i < r->settled_count && !found; i++) if (!memcmp(r->settled[i].key, key, 32)) found = r->settled + i;
    if (!found) return 0;
    if (outputs) *outputs = found->outputs;
    if (length) *length = found->length;
    return 1;
}
size_t sh_package_source_reuse_settled_count(const sh_package_source_reuse *reuse)
{ return reuse ? reuse->settled_count : 0; }
size_t sh_package_source_reuse_binding_count(const sh_package_source_reuse *reuse) { return reuse ? reuse->binding_count : 0; }
static int sr_grow(void **items, size_t *capacity, size_t count, size_t size)
{
    if (count < *capacity) return 1;
    /* By half again: a capture's binding tables hold hundreds of thousands of rows, which doubling leaves up to half
     * unused. */
    size_t next = *capacity ? *capacity + *capacity / 2 : 64;
    if (next <= *capacity || next > SIZE_MAX / size || SL_REUSE_REFUSED()) return 0;
    void *grown = realloc(*items, next * size);
    if (!grown) return 0;
    *items = grown; *capacity = next; return 1;
}
/* The record's one copy of text (NULL on allocation failure). */
static const char *sr_intern(sh_package_source_reuse *r, const char *text)
{
    if (r->string_count + 1 > r->string_capacity / 2) {
        size_t next = r->string_capacity ? r->string_capacity * 2 : 1024;
        char **grown = next <= SIZE_MAX / sizeof(*grown) && !SL_REUSE_REFUSED() ? calloc(next, sizeof(*grown)) : NULL;
        if (!grown) return NULL;
        for (size_t i = 0; i < r->string_capacity; i++) if (r->strings[i]) {
            size_t at = (size_t)sl_hash_text(0, r->strings[i]) & (next - 1);
            while (grown[at]) at = (at + 1) & (next - 1);
            grown[at] = r->strings[i];
        }
        free(r->strings); r->strings = grown; r->string_capacity = next;
    }
    size_t at = (size_t)sl_hash_text(0, text) & (r->string_capacity - 1);
    while (r->strings[at] && strcmp(r->strings[at], text)) at = (at + 1) & (r->string_capacity - 1);
    if (!r->strings[at]) {
        if (!(r->strings[at] = _strdup(text))) return NULL;
        r->string_count++;
    }
    return r->strings[at];
}
static const sl_reuse_scope *sr_scope_of(const sh_package_source_reuse *r, const unsigned char key[32], size_t *index)
{
    for (size_t i = 0; r && i < r->scope_count; i++)
        if (!memcmp(r->scopes[i].key, key, 32)) { if (index) *index = i; return r->scopes + i; }
    return NULL;
}

/* Bytes for a digest. */
typedef struct sl_bytes { unsigned char *bytes; size_t length, capacity; int failed; } sl_bytes;
static void sl_bytes_put(sl_bytes *b, const void *bytes, size_t length)
{
    if (b->failed || !length) return;
    if (length > b->capacity - b->length) {
        size_t next = b->capacity ? b->capacity : 512;
        while (next - b->length < length) {
            if (next > SIZE_MAX / 2) { b->failed = 1; return; }
            next *= 2;
        }
        unsigned char *grown = realloc(b->bytes, next);
        if (!grown) { b->failed = 1; return; }
        b->bytes = grown; b->capacity = next;
    }
    memcpy(b->bytes + b->length, bytes, length); b->length += length;
}
static void sl_bytes_u64(sl_bytes *b, uint64_t value)
{
    unsigned char v[8];
    for (int i = 0; i < 8; i++) v[i] = (unsigned char)(value >> (8 * i));
    sl_bytes_put(b, v, sizeof(v));
}
static void sl_bytes_text(sl_bytes *b, const char *text)
{
    if (!text) { sl_bytes_u64(b, UINT64_MAX); return; }
    size_t n = strlen(text);
    sl_bytes_u64(b, n); sl_bytes_put(b, text, n);
}
static int sl_bytes_digest(sl_bytes *b, unsigned char out[32])
{
    sh_package_file_identity identity;
    int ok = !b->failed && sh_package_bytes_identity(b->bytes ? b->bytes : (const unsigned char *)"", b->length, &identity);
    if (ok) memcpy(out, identity.digest, 32);
    free(b->bytes); memset(b, 0, sizeof(*b));
    return ok;
}

/* How a unit's file resolves: a captured original (by its status), its scope's own file, or anything else. */
static uint64_t sl_reuse_kind(const sh_package_source_link *s, const sl_unit *unit, const sh_compiled_resource *file)
{
    const sl_original *original = file ? sl_original_peek(s, file->engine_path) : NULL;
    if (original && &original->resource == file) return 16u + (uint64_t)(original->status + 1);
    if (file && sl_supplied_file(s, unit->scope, file)) return 1;
    return 2;
}
static void sl_reuse_file(sl_bytes *b, const sh_package_source_link *s, const sl_unit *unit, const sh_compiled_resource *file)
{
    sl_bytes_text(b, file ? file->engine_path : NULL); sl_bytes_u64(b, sl_reuse_kind(s, unit, file));
}
static void sl_reuse_target(sl_bytes *b, const sh_package_source_link *s, const sl_unit *target)
{
    sl_bytes_text(b, target->unit->family); sl_bytes_text(b, target->unit->name);
    sl_bytes_u64(b, target->scope == s->share.baseline ? 0 : 1);
}
/* The fold structure of a discovered unit (what sl_fold_same compares, with targets by name): the digest a scope's
 * binding records and its restore compares. Cached per unit for the capture. 0 on allocation failure. */
static int sl_reuse_digest_of(sh_package_source_link *s, const sl_unit *unit, unsigned char out[32])
{
    size_t mask = s->reuse.digest_capacity - 1, at = 0;
    if (s->reuse.digest_capacity) {
        at = (size_t)(((uint64_t)(uintptr_t)unit >> 4) * UINT64_C(11400714819323198485)) & mask;
        while (s->reuse.digests[at].unit) {
            if (s->reuse.digests[at].unit == unit) { memcpy(out, s->reuse.digests[at].digest, 32); return 1; }
            at = (at + 1) & mask;
        }
    }
    const sh_package_link_unit *r = unit->unit;
    const sl_discovery *d = unit->discovery;
    sl_bytes b = {0};
    sl_bytes_text(&b, "snapmap-plus scope reuse unit 1");
    sl_bytes_text(&b, r->family); sl_bytes_text(&b, r->name);
    sl_bytes_u64(&b, r->file_count);
    for (size_t i = 0; i < r->file_count; i++) sl_reuse_file(&b, s, unit, r->files[i]);
    sl_bytes_u64(&b, r->witness_count);
    for (size_t i = 0; i < r->witness_count; i++) {
        sl_bytes_text(&b, r->witnesses[i].role); sl_reuse_file(&b, s, unit, r->witnesses[i].resource);
    }
    sl_bytes_u64(&b, r->file_identities != NULL);
    sl_bytes_u64(&b, unit->discovered_count);
    for (size_t i = 0; i < unit->discovered_count; i++) sl_reuse_file(&b, s, unit, unit->discovered[i]);
    sl_bytes_u64(&b, unit->site_count);
    for (size_t i = 0; i < unit->site_count; i++) {
        sl_reuse_file(&b, s, unit, d->sites[i].file); sl_bytes_text(&b, d->sites[i].slot);
        sl_reuse_target(&b, s, d->sites[i].target);
    }
    sl_bytes_u64(&b, d->dependency_count);
    for (size_t i = 0; i < d->dependency_count; i++) sl_reuse_target(&b, s, d->dependencies[i]);
    sl_bytes_u64(&b, d->implicit_count);
    for (size_t i = 0; i < d->implicit_count; i++) {
        sl_bytes_text(&b, d->implicit[i].slot); sl_bytes_u64(&b, (uint64_t)d->implicit[i].conditional);
        sl_reuse_target(&b, s, d->implicit[i].target);
    }
    sl_bytes_text(&b, SL_EXTRA(unit)->fsm_config);
    /* What discovery sets (presentation and product come from certification, after every fold). */
    uint64_t flags = (uint64_t)unit->coverage | (uint64_t)unit->campaign_boundary << 1 | (uint64_t)d->material_members << 2;
    sl_bytes_u64(&b, flags);
    if (!r->file_count) { sl_bytes_u64(&b, (uint64_t)(int64_t)r->original_binding); sl_bytes_put(&b, r->semantics, 32); }
    if (!sl_bytes_digest(&b, out)) return 0;
    if (s->reuse.digest_count + 1 > s->reuse.digest_capacity / 2) {
        size_t next = s->reuse.digest_capacity ? s->reuse.digest_capacity * 2 : 1024;
        sl_reuse_digest *grown = next <= SIZE_MAX / sizeof(*grown) ? calloc(next, sizeof(*grown)) : NULL;
        if (!grown) return 1; /* the cache is optional */
        for (size_t i = 0; i < s->reuse.digest_capacity; i++) if (s->reuse.digests[i].unit) {
            size_t to = (size_t)(((uint64_t)(uintptr_t)s->reuse.digests[i].unit >> 4) * UINT64_C(11400714819323198485)) & (next - 1);
            while (grown[to].unit) to = (to + 1) & (next - 1);
            grown[to] = s->reuse.digests[i];
        }
        free(s->reuse.digests); s->reuse.digests = grown; s->reuse.digest_capacity = next; mask = next - 1;
        at = (size_t)(((uint64_t)(uintptr_t)unit >> 4) * UINT64_C(11400714819323198485)) & mask;
        while (s->reuse.digests[at].unit) at = (at + 1) & mask;
    }
    s->reuse.digests[at].unit = unit; memcpy(s->reuse.digests[at].digest, out, 32); s->reuse.digest_count++;
    return 1;
}

/* What a package scope's key covers: the capture context (the caller's reuse context, the catalog rows, the native
 * managers and reflection, the sharing modes and which adapters are present), the scope's attributes and every
 * field of every resource it compiled except inventory indices (their sets only by emptiness), with each payload's
 * identity. Every reader of the scope's discovery reads only these, its captured originals (each recording revalidates
 * those) and the baseline (each offered binding revalidates what it relies on there). */
static int sl_reuse_scope_key(sh_package_source_link *s, size_t scope, const sh_package_source_root_plan *plan,
    unsigned char key[32])
{
    const sh_package_compilation *c = s->scopes[scope];
    unsigned char digest[32];
    sl_bytes b = {0};
    sl_bytes_text(&b, "snapmap-plus scope reuse key 1");
    sl_bytes_put(&b, s->native.reuse_context ? s->native.reuse_context : (const unsigned char *)"\0\0\0\0\0\0\0\0"
        "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 32);
    memset(digest, 0, sizeof(digest));
    if (s->catalog && !sh_resource_catalog_digest(s->catalog, digest)) { free(b.bytes); return 0; }
    sl_bytes_put(&b, digest, 32);
    if (!sl_native_digest(s, digest)) { free(b.bytes); return 0; }
    sl_bytes_put(&b, digest, 32);
    uint64_t context = (uint64_t)s->share.mode | (uint64_t)(SL_SHARE_CONSERVATIVE != 0) << 2 | (uint64_t)(g_sl_stream != 0) << 3 |
        (uint64_t)(s->native.admit != NULL) << 4 | (uint64_t)(s->native.semantics != NULL) << 5 |
        (uint64_t)(s->native.shader != NULL) << 6 | (uint64_t)(s->native.material_companions != NULL) << 7 |
        (uint64_t)(s->native.material_seeds != NULL) << 8 | (uint64_t)(s->native.implicit != NULL) << 9 |
        (uint64_t)(s->native.owners != NULL) << 10 | (uint64_t)(s->native.audio != NULL) << 11 |
        (uint64_t)(s->native.prepare != NULL) << 12 | (uint64_t)(s->native.produce != NULL) << 13 |
        (uint64_t)(s->native.produce_derived != NULL) << 14 | (uint64_t)(s->native.availability != NULL) << 15;
    sl_bytes_u64(&b, context);
    if (s->native.shader) sl_bytes_u64(&b, (uint64_t)s->native.shader->renderer);
    sl_canonicalizer own = sl_canonicalizer_of(c), base = sl_canonicalizer_of(s->scopes[s->share.baseline]);
    uint64_t attributes = (uint64_t)(s->share.fold && s->share.fold[scope]) |
        (uint64_t)(plan && plan->discover && plan->discover[scope]) << 1 |
        (uint64_t)(s->share.fsm_inputs && s->share.fsm_inputs[scope]) << 2 |
        (uint64_t)(own == base ? 1 : own ? 2 : 0) << 3 | (uint64_t)(c->independent_sources != 0) << 5 |
        (uint64_t)(c->map_overlay != 0) << 6 | (uint64_t)(c->map_independent_sources != 0) << 7;
    sl_bytes_u64(&b, attributes);
    /* An unrecognized canonicalizer is only this process's: such a key matches nothing elsewhere. */
    if (own && own != base) sl_bytes_u64(&b, (uint64_t)(uintptr_t)own);
    {
        size_t length = 0;
        char *text = sh_json_serialize_object(&c->policy.requirements, 0, &length);
        if (!text) { free(b.bytes); return 0; }
        sl_bytes_text(&b, text); free(text);
    }
    sl_bytes_u64(&b, c->resource_count);
    for (size_t i = 0; i < c->resource_count; i++) {
        const sh_compiled_resource *r = c->resources + i;
        sh_package_file_identity identity;
        sl_bytes_text(&b, r->engine_path); sl_bytes_text(&b, r->type); sl_bytes_text(&b, r->name);
        sl_bytes_text(&b, r->native_type); sl_bytes_text(&b, r->native_name); sl_bytes_text(&b, r->original_path);
        uint64_t flags = (uint64_t)(r->baseline_known & 0xff) | (uint64_t)(r->composed != 0) << 8 |
            (uint64_t)(r->native_original != 0) << 9 | (uint64_t)(r->restored_original != 0) << 10 |
            (uint64_t)(r->semantic_coverage != 0) << 11 | (uint64_t)(r->generated != 0) << 12 |
            (uint64_t)(r->generated_layer != 0) << 13 | (uint64_t)(r->rebound_original != 0) << 14 |
            (uint64_t)(r->owns_engine_path != 0) << 15 | (uint64_t)(r->source_count != 0) << 16 |
            (uint64_t)(sh_package_owners_count(&r->owners) != 0) << 17 |
            (uint64_t)(sh_package_owners_count(&r->gameplay_owners) != 0) << 18 | (uint64_t)(r->cache_path != NULL) << 19;
        sl_bytes_u64(&b, flags);
        sl_bytes_u64(&b, (uint64_t)r->sound_authored_event); sl_bytes_u64(&b, (uint64_t)r->sound_event);
        sl_bytes_u64(&b, r->generated_input_count);
        for (size_t k = 0; k < r->generated_input_count; k++) sl_bytes_text(&b, r->generated_inputs[k]);
        sl_bytes_u64(&b, r->bound_dependency_count);
        for (size_t k = 0; k < r->bound_dependency_count; k++) sl_bytes_text(&b, r->bound_dependencies[k]);
        sl_bytes_u64(&b, r->native_alias_count);
        for (size_t k = 0; k < r->native_alias_count; k++) {
            sl_bytes_text(&b, r->native_aliases[k].type); sl_bytes_text(&b, r->native_aliases[k].name);
        }
        if (!sh_package_compilation_identity(c, r, &identity, NULL, 0)) { free(b.bytes); return 0; }
        sl_bytes_u64(&b, identity.length); sl_bytes_put(&b, identity.digest, 32);
        if (!sh_package_bytes_identity(r->presented ? r->presented : (const unsigned char *)"", r->presented_length, &identity)) {
            free(b.bytes); return 0;
        }
        sl_bytes_u64(&b, r->presented_length); sl_bytes_put(&b, identity.digest, 32);
    }
    return sl_bytes_digest(&b, key);
}

/* The set of paths a recording consulted that its scope does not supply (its files and lookups), once per
 * recording: 1 + its index, 0 on allocation failure (the environment failure is recorded). */
static uint32_t sl_reuse_set_of(sh_package_source_link *s, const sl_replay *r)
{
    uint64_t hash = ((uint64_t)(uintptr_t)r >> 4) * UINT64_C(11400714819323198485) ^ r->serial;
    size_t at = 0;
    if (s->reuse.set_slot_capacity) {
        at = (size_t)hash & (s->reuse.set_slot_capacity - 1);
        while (s->reuse.set_slots[at]) {
            const sl_reuse_set *set = s->reuse.sets + s->reuse.set_slots[at] - 1;
            if (set->replay == r && set->serial == r->serial) return s->reuse.set_slots[at];
            at = (at + 1) & (s->reuse.set_slot_capacity - 1);
        }
    }
    if (s->reuse.set_count >= UINT32_MAX - 1 ||
        !sr_grow((void **)&s->reuse.sets, &s->reuse.set_capacity, s->reuse.set_count, sizeof(*s->reuse.sets))) goto failed;
    if (s->reuse.set_count + 1 > s->reuse.set_slot_capacity / 2) {
        size_t next = s->reuse.set_slot_capacity ? s->reuse.set_slot_capacity * 2 : 4096;
        uint32_t *grown = next <= SIZE_MAX / sizeof(*grown) ? calloc(next, sizeof(*grown)) : NULL;
        if (!grown) goto failed;
        for (size_t i = 0; i < s->reuse.set_count; i++) {
            const sl_reuse_set *set = s->reuse.sets + i;
            uint64_t h = ((uint64_t)(uintptr_t)set->replay >> 4) * UINT64_C(11400714819323198485) ^ set->serial;
            size_t to = (size_t)h & (next - 1);
            while (grown[to]) to = (to + 1) & (next - 1);
            grown[to] = (uint32_t)(i + 1);
        }
        free(s->reuse.set_slots); s->reuse.set_slots = grown; s->reuse.set_slot_capacity = next;
        at = (size_t)hash & (next - 1);
        while (s->reuse.set_slots[at]) at = (at + 1) & (next - 1);
    }
    sl_reuse_set *set = s->reuse.sets + s->reuse.set_count;
    set->replay = r; set->serial = r->serial; set->first = s->reuse.path_count; set->count = 0; set->supplied = 0;
    set->baseline = 0;
    for (int part = 0; part < 2; part++) {
        const sl_replay_input *inputs = part ? r->lookups : r->files;
        size_t count = part ? r->lookup_count : r->file_count;
        for (size_t i = 0; i < count; i++) {
            if (inputs[i].state < 0) { set->supplied = 1; continue; }
            const char *path = sl_intern(s, inputs[i].path);
            if (!path || !sr_grow((void **)&s->reuse.paths, &s->reuse.path_capacity, s->reuse.path_count,
                sizeof(*s->reuse.paths))) goto failed;
            if (s->reuse.path_mark_capacity < s->reuse.path_capacity) {
                unsigned char *grown = realloc(s->reuse.path_marks, s->reuse.path_capacity);
                if (!grown) goto failed;
                s->reuse.path_marks = grown; s->reuse.path_mark_capacity = s->reuse.path_capacity;
            }
            s->reuse.path_marks[s->reuse.path_count] = 0;
            s->reuse.paths[s->reuse.path_count++] = path; set->count++;
        }
    }
    s->reuse.set_slots[at] = (uint32_t)++s->reuse.set_count;
    return s->reuse.set_slots[at];
failed:
    sl_env_fail(s, "cannot record a scope's reuse inputs", r->name); return 0;
}
/* A binding a fold or a restore made: kept for the record when the capture completes. */
static int sl_reuse_mark(sh_package_source_link *s, size_t scope, sl_unit *stock, uint32_t set, const sl_reuse_stock *from)
{
    if (scope > UINT32_MAX ||
        !sr_grow((void **)&s->reuse.marks, &s->reuse.mark_capacity, s->reuse.mark_count, sizeof(*s->reuse.marks)))
        return sl_env_fail(s, "cannot record a scope's shared stock binding", stock->unit->name);
    s->reuse.marks[s->reuse.mark_count++] = (sl_reuse_note){.stock = stock, .from = from, .scope = (uint32_t)scope,
        .set = set, .pass = s->reuse.pass};
    return 1;
}

static sl_reuse_candidate *sl_reuse_candidate_of(const sh_package_source_link *s, const char *family, const char *name)
{
    if (!s->reuse.candidate_slot_capacity) return NULL;
    size_t at = (size_t)sl_hash_text(sl_hash_text(UINT64_C(14695981039346656037), family), name) &
        (s->reuse.candidate_slot_capacity - 1);
    while (s->reuse.candidate_slots[at]) {
        sl_reuse_candidate *c = s->reuse.candidates + s->reuse.candidate_slots[at] - 1;
        if (!strcmp(c->record->family, family) && !strcmp(c->record->name, name)) return c;
        at = (at + 1) & (s->reuse.candidate_slot_capacity - 1);
    }
    return NULL;
}
static sl_unit *sl_reuse_bound(const sh_package_source_link *s, const char *family, const char *name)
{
    const sl_reuse_candidate *c = sl_reuse_candidate_of(s, family, name);
    return c && c->state == SL_REUSE_BOUND ? c->pool : NULL;
}
static int sl_reuse_unmarked(const sh_package_source_link *s, const sl_unit *target)
{
    if (s->reuse.table_scope == SIZE_MAX || target->scope != s->share.baseline) return 0;
    const sl_reuse_candidate *c = sl_reuse_candidate_of(s, target->unit->family, target->unit->name);
    return c && c->state == SL_REUSE_BOUND && c->pool == target;
}
static int sl_reuse_restoring(const sh_package_source_link *s, size_t scope)
{
    return s->reuse.in && s->reuse.matched && scope < s->scope_count && s->reuse.matched[scope] != SIZE_MAX;
}
enum { SL_REUSE_OWN, SL_REUSE_ABSENT, SL_REUSE_STRUCTURE, SL_REUSE_FAMILY, SL_REUSE_CHANGED, SL_REUSE_INPUTS,
    SL_REUSE_CLOSURE, SL_REUSE_CERTIFICATE };
static int sl_reuse_refuse(sh_package_source_link *s, sl_reuse_candidate *c, int reason)
{
    if (SL_REUSE_DEBUG()) fprintf(stderr, "scope reuse: scope %zu pass %u refuses %s %s (reason %d)\n",
        s->reuse.table_scope, s->reuse.pass, c->record->family, c->record->name, reason);
    c->state = SL_REUSE_REFUSED; c->reason = reason; s->reuse.counts.refused[reason]++; return 1;
}
/* Whether scope or the baseline supplies path (either way, the scope's copy would not be the baseline's). */
static int sl_reuse_supplied(sh_package_source_link *s, size_t scope, const char *path)
{
    return sl_lookup_supplied(s, s->share.baseline, path) || sl_lookup_supplied(s, scope, path);
}
/* The same for a captured original's path, through its mark (a restored scope keys paths as the baseline does). */
static int sl_reuse_original_supplied(sh_package_source_link *s, size_t scope, sl_original *original)
{
    return sl_lookup_marked(s, s->share.baseline, original->resource.engine_path, &original->mark) ||
        sl_lookup_marked(s, scope, original->resource.engine_path, &original->mark);
}
/* Judge one offered binding (sl_reuse_begin). 0 only for an environment failure. */
static int sl_reuse_judge(sh_package_source_link *s, size_t scope, sl_reuse_candidate *c)
{
    const sl_reuse_stock *record = c->record;
    sl_unit *own = sl_find_raw(s, scope, record->family, record->name);
    if (own) {
        /* Only an admission copy of the first pass, not discovered yet, is replaced (as the fold replaces it). */
        if (!SL_REUSE_MUTATED(5) && (s->reuse.pass != 1 || !own->discovery || own->discovery->discovered_done ||
            own->discovery->seeded || own->discovery->nominated)) return sl_reuse_refuse(s, c, SL_REUSE_OWN);
        c->copy = own;
    }
    sl_unit *pool = sl_find_raw(s, s->share.baseline, record->family, record->name);
    if (!pool || !pool->discovery || !pool->discovery->discovered_done) return sl_reuse_refuse(s, c, SL_REUSE_ABSENT);
    c->pool = pool;
    if (SL_REUSE_MUTATED(7)) { c->state = SL_REUSE_VALID; return 1; } /* mutation control: the record trusted */
    const sh_package_link_unit *r = pool->unit;
    if (pool->coverage || pool->campaign_boundary || pool->presentation || pool->discovery->product ||
        pool->discovery->nominated || SL_EXTRA(pool)->prepared || SL_EXTRA(pool)->program_count ||
        SL_EXTRA(pool)->shader_file_count || SL_EXTRA(pool)->identities || r->file_identities)
        return sl_reuse_refuse(s, c, SL_REUSE_STRUCTURE);
    if (!sl_share_family(s, pool)) return sl_reuse_refuse(s, c, SL_REUSE_FAMILY);
    if (!SL_SHARE_CONSERVATIVE) {
        /* A conductor stays its scope's own (sl_fold_scope); conservatively its stock fact refuses it (the certificate). */
        int conductor = sl_conductor(s, pool);
        if (conductor < 0) return sl_env_fail(s, "cannot read a shared stock declaration", record->name);
        if (conductor) return sl_reuse_refuse(s, c, SL_REUSE_FAMILY);
    }
    unsigned char digest[32];
    if (!sl_reuse_digest_of(s, pool, digest)) return sl_env_fail(s, "cannot identify shared stock", record->name);
    if (memcmp(digest, record->digest, 32) && !SL_REUSE_MUTATED(1)) return sl_reuse_refuse(s, c, SL_REUSE_CHANGED);
    /* Its files are the captured originals the baseline would read too (sl_fold_structure), and what its copy
     * consulted resolves to them alike (sl_share_inputs). */
    for (size_t i = 0; i < r->file_count + r->witness_count; i++) {
        const sh_compiled_resource *file = i < r->file_count ? r->files[i] : r->witnesses[i - r->file_count].resource;
        sl_original *original = file ? sl_original_peek(s, file->engine_path) : NULL;
        if (!original || &original->resource != file || original->status != 1) return sl_reuse_refuse(s, c, SL_REUSE_STRUCTURE);
        if (sl_reuse_original_supplied(s, scope, original) && !SL_REUSE_MUTATED(2)) return sl_reuse_refuse(s, c, SL_REUSE_INPUTS);
    }
    if (r->file_count && !SL_REUSE_MUTATED(2)) {
        /* The scope's copy would read what the stock unit's discovery read, resolved alike: every path its recording
         * consulted is a captured original that neither the scope nor the baseline supplies (sl_replay_applies,
         * sl_share_inputs). A unit discovered without a recording establishes nothing. */
        uint32_t consulted = pool->discovery->consulted;
        if (!consulted) return sl_reuse_refuse(s, c, SL_REUSE_INPUTS);
        sl_reuse_set *set = s->reuse.sets + consulted - 1;
        if (set->supplied) return sl_reuse_refuse(s, c, SL_REUSE_INPUTS);
        if (!set->baseline) {
            set->baseline = 1;
            for (size_t i = 0; i < set->count && set->baseline == 1; i++)
                if (sl_lookup_marked(s, s->share.baseline, s->reuse.paths[set->first + i],
                    s->reuse.path_marks + set->first + i)) set->baseline = 2;
        }
        if (set->baseline == 2) return sl_reuse_refuse(s, c, SL_REUSE_INPUTS);
        for (size_t i = 0; i < set->count; i++)
            if (sl_lookup_marked(s, scope, s->reuse.paths[set->first + i], s->reuse.path_marks + set->first + i))
                return sl_reuse_refuse(s, c, SL_REUSE_INPUTS);
        /* And file selection there would choose the same files: no permutation or declaration path of the unit is
         * the scope's or the baseline's (sl_files). */
        const sh_resource_catalog_entry *const *rows = NULL;
        size_t count = sh_resource_catalog_find(s->catalog, r->family, r->name, &rows);
        for (size_t i = 0; i < count; i++)
            if (*rows[i]->path && sl_reuse_supplied(s, scope, rows[i]->path)) return sl_reuse_refuse(s, c, SL_REUSE_INPUTS);
        if (!count) {
            char *path = sh_package_cooked_path(r->family, r->name);
            if (!path) path = sl_decl_path(r->family, r->name);
            int supplied = !path || sl_reuse_supplied(s, scope, path);
            free(path);
            if (supplied) return sl_reuse_refuse(s, c, SL_REUSE_INPUTS);
        }
    }
    if (!r->file_count) {
        const sh_resource_catalog_entry *const *rows = NULL;
        size_t count = sh_resource_catalog_find(s->catalog, r->family, r->name, &rows);
        if (r->original_binding != 1) return sl_reuse_refuse(s, c, SL_REUSE_STRUCTURE);
        for (size_t i = 0; i < count; i++) if (*rows[i]->path) return sl_reuse_refuse(s, c, SL_REUSE_INPUTS);
        if (!count) {
            char *path = sh_package_cooked_path(r->family, r->name);
            if (!path) path = sl_decl_path(r->family, r->name);
            int supplied = !path || sl_reuse_supplied(s, scope, path);
            free(path);
            if (supplied) return sl_reuse_refuse(s, c, SL_REUSE_INPUTS);
        }
    }
    if (!strcmp(r->family, "aibehavior") && SL_EXTRA(pool)->fsm_config &&
        (s->share.fsm_inputs[scope] || s->share.fsm_inputs[s->share.baseline])) return sl_reuse_refuse(s, c, SL_REUSE_INPUTS);
    if (!r->file_count) {
        /* The scope's copy admits its name as sl_files would there; the fold joins only the same semantics. */
        const sh_resource_catalog_entry *const *rows = NULL;
        size_t count = sh_resource_catalog_find(s->catalog, r->family, r->name, &rows);
        int registered = 0;
        for (size_t i = 0; i < count; i++) registered |= !*rows[i]->path && rows[i]->archive < 2;
        if (!registered) {
            unsigned char admitted[32];
            char error[512] = "";
            int status = s->native.admit ? s->native.admit(s->native.context, scope, r->family, r->name, admitted, error,
                sizeof(error)) : 0;
            if (status < 0) return sl_env_fail(s, *error ? error : "cannot admit a recorded stock name", r->name);
            if (status != 1 || memcmp(admitted, r->semantics, 32)) return sl_reuse_refuse(s, c, SL_REUSE_CERTIFICATE);
        }
    }
    c->state = SL_REUSE_VALID;
    return 1;
}
/* The native implicit adapter's requests for one unit, in call order (sl_reuse_certify). */
typedef struct sl_reuse_request_entry { char *slot, *family, *name; } sl_reuse_request_entry;
typedef struct sl_reuse_requests { sl_reuse_request_entry *items; size_t count, capacity; int failed; } sl_reuse_requests;
static int sl_reuse_request(void *context, const char *slot, const char *family, const char *name)
{
    sl_reuse_requests *r = context;
    if (r->failed) return 1;
    if (!sr_grow((void **)&r->items, &r->capacity, r->count, sizeof(*r->items))) { r->failed = 1; return 1; }
    sl_reuse_request_entry *e = r->items + r->count;
    e->slot = _strdup(slot ? slot : ""); e->family = _strdup(family ? family : ""); e->name = _strdup(name ? name : "");
    if (!e->slot || !e->family || !e->name) { free(e->slot); free(e->family); free(e->name); r->failed = 1; return 1; }
    r->count++; return 1;
}
static int sl_reuse_request_order(const void *a, const void *b)
{
    const sl_reuse_request_entry *x = a, *y = b;
    int order = strcmp(x->slot, y->slot);
    if (!order) order = strcmp(x->family, y->family);
    return order ? order : strcmp(x->name, y->name);
}
static int sl_reuse_requests_same(sl_reuse_requests *a, sl_reuse_requests *b)
{
    if (a->count != b->count) return 0;
    if (a->count > 1) { qsort(a->items, a->count, sizeof(*a->items), sl_reuse_request_order);
        qsort(b->items, b->count, sizeof(*b->items), sl_reuse_request_order); }
    for (size_t i = 0; i < a->count; i++) if (sl_reuse_request_order(a->items + i, b->items + i)) return 0;
    return 1;
}
static void sl_reuse_requests_free(sl_reuse_requests *r)
{
    for (size_t i = 0; i < r->count; i++) { free(r->items[i].slot); free(r->items[i].family); free(r->items[i].name); }
    free(r->items); memset(r, 0, sizeof(*r));
}
/* The certificate of a structurally valid binding, asked as the fold asks it for the scope's copy: the stock unit's
 * record read through the scope, with the stock unit as known when a fold or a binding certified it. 0 only for an
 * environment failure. */
static int sl_reuse_certify(sh_package_source_link *s, size_t scope, sl_reuse_candidate *c)
{
    sl_unit *pool = c->pool;
    c->certify = !pool->fold_certified;
    if (SL_REUSE_MUTATED(4) || SL_REUSE_MUTATED(7)) { memcpy(c->semantics, c->record->semantics, 32); return 1; }
    sl_unit copy = *pool;
    sh_package_link_unit row = *pool->unit;
    row.compilation = s->scopes[scope]; copy.unit = &row; copy.scope = (uint32_t)scope;
    int keep = sl_fold_certificate(s, scope, &copy, pool->fold_certified ? pool : NULL, c->semantics);
    if (keep < 0) return sl_env_fail(s, "cannot certify shared stock", c->record->name);
    if (keep != SL_KEEP_NONE) return sl_reuse_refuse(s, c, SL_REUSE_CERTIFICATE);
    /* The native implicit adapter may answer per scope: it requests for the scope's copy exactly what it requests
     * for the stock unit, or the copy is discovered (its requests are part of the structure the fold compares). */
    if (s->native.implicit && !SL_REUSE_MUTATED(6)) {
        sl_reuse_requests mine = {0}, theirs = {0};
        char error[512] = "";
        int asked = s->native.implicit(s->native.context, scope, s->observe, &row, sl_reuse_request, &mine, error,
            sizeof(error)) == 1 && s->native.implicit(s->native.context, s->share.baseline, s->observe, pool->unit,
            sl_reuse_request, &theirs, error, sizeof(error)) == 1;
        int same = asked && !mine.failed && !theirs.failed && sl_reuse_requests_same(&mine, &theirs);
        int failed = mine.failed || theirs.failed;
        sl_reuse_requests_free(&mine); sl_reuse_requests_free(&theirs);
        if (failed) return sl_env_fail(s, "cannot compare native requests", c->record->name);
        if (!same) return sl_reuse_refuse(s, c, SL_REUSE_CERTIFICATE);
    }
    return 1;
}
/* Offer scope its record's bindings of this pass (see above). 0 only for an environment failure. */
static int sl_reuse_begin(sh_package_source_link *s, size_t scope)
{
    unsigned long long began = sl_now_us();
    const sl_reuse_scope *rs = s->reuse.in->scopes + s->reuse.matched[scope];
    s->reuse.candidate_count = 0;
    if ((s->withdrawn && s->withdrawn[scope]) || *s->preparation_reason || s->deferred.unit || s->failed) return 1;
    size_t offered = 0;
    for (size_t i = 0; i < rs->count; i++) offered += s->reuse.in->bindings[rs->first + i].pass == s->reuse.pass;
    if (!offered) return 1;
    if (offered > s->reuse.candidate_capacity) {
        sl_reuse_candidate *grown = offered <= SIZE_MAX / sizeof(*grown) && !SL_REUSE_REFUSED() ?
            realloc(s->reuse.candidates, offered * sizeof(*grown)) : NULL;
        if (!grown) return sl_env_fail(s, "cannot offer a scope's recorded shared stock", NULL);
        s->reuse.candidates = grown; s->reuse.candidate_capacity = offered;
    }
    size_t slots = 64;
    while (slots < offered * 2) slots *= 2;
    if (slots > s->reuse.candidate_slot_capacity) {
        uint32_t *grown = SL_REUSE_REFUSED() ? NULL : calloc(slots, sizeof(*grown));
        if (!grown) return sl_env_fail(s, "cannot offer a scope's recorded shared stock", NULL);
        free(s->reuse.candidate_slots); s->reuse.candidate_slots = grown; s->reuse.candidate_slot_capacity = slots;
    } else memset(s->reuse.candidate_slots, 0, s->reuse.candidate_slot_capacity * sizeof(*s->reuse.candidate_slots));
    s->reuse.table_scope = scope;
    s->reuse.counts.scopes_restored += s->reuse.pass == 1;
    for (size_t i = 0; i < rs->count; i++) {
        const sl_reuse_binding *binding = s->reuse.in->bindings + rs->first + i;
        if (binding->pass != s->reuse.pass) continue;
        const sl_reuse_stock *record = s->reuse.in->stock + binding->stock;
        if (sl_reuse_candidate_of(s, record->family, record->name)) continue; /* one binding a name */
        size_t at = (size_t)sl_hash_text(sl_hash_text(UINT64_C(14695981039346656037), record->family), record->name) &
            (s->reuse.candidate_slot_capacity - 1);
        while (s->reuse.candidate_slots[at]) at = (at + 1) & (s->reuse.candidate_slot_capacity - 1);
        sl_reuse_candidate *c = s->reuse.candidates + s->reuse.candidate_count;
        memset(c, 0, sizeof(*c)); c->record = record;
        s->reuse.candidate_slots[at] = (uint32_t)++s->reuse.candidate_count;
        s->reuse.counts.bindings_offered++;
        if (!sl_reuse_judge(s, scope, c)) return 0;
    }
    /* As the fold decides: the closure (a binding whose stock unit depends on a unit the scope does not bind stays
     * unbound, and so does what depends on it), then the certificate of what it keeps, until nothing changes. */
    unsigned char *checked = calloc(s->reuse.candidate_count ? s->reuse.candidate_count : 1, 1);
    if (!checked || SL_REUSE_REFUSED()) { free(checked); return sl_env_fail(s, "cannot offer a scope's recorded shared stock", NULL); }
    for (int again = 1; again;) {
        again = 0;
        for (int changed = 1; changed && !SL_REUSE_MUTATED(3) && !SL_REUSE_MUTATED(7);) {
            changed = 0;
            for (size_t i = 0; i < s->reuse.candidate_count; i++) {
                sl_reuse_candidate *c = s->reuse.candidates + i;
                if (c->state != SL_REUSE_VALID) continue;
                const sl_discovery *d = c->pool->discovery;
                for (uint32_t e = 0; e < d->dependency_count; e++) {
                    sl_unit *target = d->dependencies[e];
                    if (target->scope != s->share.baseline) { sl_reuse_refuse(s, c, SL_REUSE_CLOSURE); changed = 1; break; }
                    if (sl_share_find(s, scope, target->unit->family, target->unit->name) == target) continue;
                    const sl_reuse_candidate *t = sl_reuse_candidate_of(s, target->unit->family, target->unit->name);
                    if (t && t->state == SL_REUSE_VALID && t->pool == target) continue;
                    sl_reuse_refuse(s, c, SL_REUSE_CLOSURE); changed = 1; break;
                }
            }
        }
        for (size_t i = 0; i < s->reuse.candidate_count; i++) {
            sl_reuse_candidate *c = s->reuse.candidates + i;
            if (c->state != SL_REUSE_VALID || checked[i]) continue;
            checked[i] = 1;
            if (!sl_reuse_certify(s, scope, c)) { free(checked); return 0; }
            if (c->state != SL_REUSE_VALID) again = 1;
        }
    }
    free(checked);
    for (size_t i = 0; i < s->reuse.candidate_count; i++) s->reuse.counts.bindings_valid += s->reuse.candidates[i].state == SL_REUSE_VALID;
    /* Admission copies the valid bindings replace: the scope's discovered units bind the stock units instead, the
     * copies go (sl_unit_drop) and leave the scope's pending list. */
    size_t copies = 0;
    for (size_t i = 0; i < s->reuse.candidate_count; i++) {
        sl_reuse_candidate *c = s->reuse.candidates + i;
        if (c->state != SL_REUSE_VALID || !c->copy) { c->copy = NULL; continue; }
        if (SL_REUSE_MUTATED(5) && c->copy->discovery->discovered_done) {
            /* Mutation control: the scope's own unit stays, and the record's binding is bound beside it. */
            c->copy = NULL;
            if (!sl_reuse_bind(s, scope, c->record->family, c->record->name)) return 0;
            continue;
        }
        c->copy->discovery->fold_slot = (uint32_t)(i + 1); copies++;
    }
    if (copies) {
        sl_pending *pending = s->share.pending + scope;
#define SL_REUSE_TO(target) ((target)->discovery && (target)->discovery->fold_slot && (target)->scope == scope ? \
    s->reuse.candidates[(target)->discovery->fold_slot - 1].pool : (target))
        for (size_t k = 0; k < pending->admitted_count; k++) {
            sl_unit *unit = pending->admitted[k];
            sl_discovery *d = unit->discovery;
            for (uint32_t e = 0; e < d->dependency_count; e++) d->dependencies[e] = SL_REUSE_TO(d->dependencies[e]);
            for (uint32_t e = 0; e < unit->site_count; e++) d->sites[e].target = SL_REUSE_TO(d->sites[e].target);
            for (uint32_t e = 0; e < d->implicit_count; e++) d->implicit[e].target = SL_REUSE_TO(d->implicit[e].target);
        }
#undef SL_REUSE_TO
        for (size_t j = pending->head; j < pending->count; j++) {
            sl_unit *unit = pending->units[j];
            if (unit && unit->discovery && unit->discovery->fold_slot) pending->units[j] = NULL;
        }
        for (size_t i = 0; i < s->reuse.candidate_count; i++) {
            sl_reuse_candidate *c = s->reuse.candidates + i;
            if (!c->copy) continue;
            sl_unit *copy = c->copy;
            c->copy = NULL;
            copy->discovery->fold_slot = 0;
            sl_unit_drop(s, copy); s->reuse.counts.copies_dropped++;
            if (!sl_reuse_bind(s, scope, c->record->family, c->record->name)) {
                if (!s->failed) sl_env_fail(s, "cannot bind recorded shared stock", c->record->name);
                return 0;
            }
        }
    }
    s->reuse.counts.validate_us += sl_now_us() - began;
    return !s->failed;
}
/* Bind an offered valid binding with its closure, as the fold binds an equal copy; NULL when none is offered for
 * family and name (or on an environment failure, recorded). */
static sl_unit *sl_reuse_bind(sh_package_source_link *s, size_t scope, const char *family, const char *name)
{
    sl_reuse_candidate *root = sl_reuse_candidate_of(s, family, name);
    if (!root) return NULL;
    if (root->state == SL_REUSE_BOUND) return root->pool;
    if (root->state != SL_REUSE_VALID) return NULL;
    sl_reuse_candidate **stack = NULL;
    size_t depth = 0, room = 0;
    sl_share_map *map = s->share.maps + scope;
    if (!sr_grow((void **)&stack, &room, depth, sizeof(*stack))) goto failed;
    stack[depth++] = root;
    while (depth) {
        sl_reuse_candidate *c = stack[--depth];
        if (c->state == SL_REUSE_BOUND) continue;
        if (c->state != SL_REUSE_VALID || SL_REUSE_REFUSED()) goto failed;
        sl_unit *pool = c->pool;
        if (!sl_units_push(&map->units, &map->count, &map->capacity, pool) ||
            (s->reuse.out && !sl_reuse_mark(s, scope, pool, 0, c->record))) goto failed;
        c->state = SL_REUSE_BOUND; s->reuse.counts.bindings_bound++;
        if (SL_REUSE_DEBUG()) fprintf(stderr, "scope reuse: scope %zu pass %u binds %s %s (stock unit %s, certify %d)\n",
            scope, s->reuse.pass, c->record->family, c->record->name, pool->stock_only ? "stock only" : "baseline member", c->certify);
        if (s->native.reuse_bound) ++*s->native.reuse_bound;
        if (c->certify && !pool->fold_certified) {
            memcpy(pool->unit->semantics, c->semantics, 32); pool->unit->original_binding = 1; pool->fold_certified = 1;
            s->reuse.counts.certified++;
        }
        if (!pool->shared_stock) s->share.stock_units++;
        pool->shared_stock = 1;
        /* As the scope's copy's recorded discovery would have (sl_discover_unit_requests), the bound stock unit's
         * backed originals let their bytes go; a later reader reads them again exactly through their shared bodies. */
        if (g_sl_stream && !sl_stream_release(s, pool)) goto failed;
        const sl_discovery *d = pool->discovery;
        for (uint32_t e = 0; e < d->dependency_count; e++) {
            sl_unit *target = d->dependencies[e];
            sl_reuse_candidate *t = sl_reuse_candidate_of(s, target->unit->family, target->unit->name);
            if (t && t->pool == target && (t->state == SL_REUSE_VALID || t->state == SL_REUSE_BOUND)) {
                if (t->state == SL_REUSE_VALID) {
                    if (!sr_grow((void **)&stack, &room, depth, sizeof(*stack))) goto failed;
                    stack[depth++] = t;
                }
                continue;
            }
            /* Bound by an earlier pass; the closure check admitted nothing else. */
            if (!SL_REUSE_MUTATED(3) && !SL_REUSE_MUTATED(7) &&
                sl_share_find(s, scope, target->unit->family, target->unit->name) != target) goto failed;
        }
    }
    free(stack);
    return root->pool;
failed:
    free(stack);
    if (!s->failed) sl_env_fail(s, "cannot bind recorded shared stock", name);
    return NULL;
}
/* The pass of a restored scope is discovered: its map is sorted again, and its offers end. */
static void sl_reuse_end(sh_package_source_link *s, size_t scope)
{
    sl_share_map *map = s->share.maps + scope;
    if (s->reuse.table_scope != scope) return;
    if (map->count > map->sorted) qsort(map->units, map->count, sizeof(*map->units), sl_share_order);
    map->sorted = map->count;
    s->reuse.table_scope = SIZE_MAX; s->reuse.candidate_count = 0;
}
/* The capture completed: its record, from the bindings each keyed scope's final map holds, in the pass that bound
 * them; one stock entry per stock unit and consulted inputs. Before the discovery state goes (the digests read it).
 * 0 only for an environment failure. */
typedef struct sl_reuse_entry_key { const sl_unit *unit; uint32_t set; const sl_reuse_stock *from; } sl_reuse_entry_key;
static size_t sl_reuse_entry_hash(const sl_unit *unit, uint32_t set, const sl_reuse_stock *from)
{
    uint64_t h = ((uint64_t)(uintptr_t)unit >> 4) * UINT64_C(11400714819323198485);
    h ^= from ? ((uint64_t)(uintptr_t)from >> 3) * UINT64_C(0x9e3779b97f4a7c15) : (uint64_t)set * UINT64_C(0xc2b2ae3d27d4eb4f);
    return (size_t)(h ^ (h >> 31));
}
static int sl_reuse_build(sh_package_source_link *s)
{
    unsigned long long began = sl_now_us();
    sh_package_source_reuse *r = sh_package_source_reuse_open();
    size_t *order = NULL, *starts = NULL, *slots = NULL, slot_capacity = 0, key_capacity = 0;
    sl_reuse_entry_key *keys = NULL;
    sl_unit **members = NULL;
    size_t member_capacity = 0;
    int ok = 0;
    if (!r || SL_REUSE_REFUSED()) goto done;
    /* Marks by scope, in order (a counting sort). */
    starts = calloc(s->scope_count + 1, sizeof(*starts));
    order = malloc((s->reuse.mark_count ? s->reuse.mark_count : 1) * sizeof(*order));
    if (!starts || !order) goto done;
    for (size_t i = 0; i < s->reuse.mark_count; i++) starts[s->reuse.marks[i].scope + 1]++;
    for (size_t k = 0; k < s->scope_count; k++) starts[k + 1] += starts[k];
    {
        size_t *cursor = malloc((s->scope_count ? s->scope_count : 1) * sizeof(*cursor));
        if (!cursor) goto done;
        memcpy(cursor, starts, s->scope_count * sizeof(*cursor));
        for (size_t i = 0; i < s->reuse.mark_count; i++) order[cursor[s->reuse.marks[i].scope]++] = i;
        free(cursor);
    }
    for (size_t scope = 0; scope < s->scope_count; scope++) {
        if (!s->reuse.keyed || !s->reuse.keyed[scope] || scope == s->share.baseline) continue;
        const sl_share_map *map = s->share.maps + scope;
        /* The scope's final map (privatized bindings left it): membership by unit, each bound once. */
        size_t count = map->count, capacity = 64;
        while (capacity < count * 2 + 2) capacity *= 2;
        if (capacity > member_capacity) {
            sl_unit **grown = realloc(members, capacity * sizeof(*grown));
            if (!grown) goto done;
            members = grown; member_capacity = capacity;
        }
        memset(members, 0, capacity * sizeof(*members));
        for (size_t j = 0; j < count; j++) {
            size_t at = (size_t)(((uint64_t)(uintptr_t)map->units[j] >> 4) * UINT64_C(11400714819323198485)) & (capacity - 1);
            while (members[at] && members[at] != map->units[j]) at = (at + 1) & (capacity - 1);
            members[at] = map->units[j];
        }
        if (!sr_grow((void **)&r->scopes, &r->scope_capacity, r->scope_count, sizeof(*r->scopes))) goto done;
        sl_reuse_scope *rs = r->scopes + r->scope_count++;
        memcpy(rs->key, s->reuse.keys[scope], 32); rs->first = r->binding_count; rs->count = 0;
        for (size_t m = starts[scope]; m < starts[scope + 1]; m++) {
            const sl_reuse_note *mark = s->reuse.marks + order[m];
            size_t at = (size_t)(((uint64_t)(uintptr_t)mark->stock >> 4) * UINT64_C(11400714819323198485)) & (capacity - 1);
            while (members[at] && members[at] != mark->stock) at = (at + 1) & (capacity - 1);
            if (members[at] != mark->stock) continue; /* privatized later, or bound already */
            members[at] = (sl_unit *)(uintptr_t)1;
            /* Its stock entry, shared by every binding of the same unit with the same inputs. */
            if (r->stock_count + 1 > slot_capacity / 2) {
                size_t next = slot_capacity ? slot_capacity * 2 : 4096;
                size_t *grown = SL_REUSE_REFUSED() ? NULL : calloc(next, sizeof(*grown));
                if (!grown) goto done;
                for (size_t e = 0; e < r->stock_count; e++) {
                    size_t to = sl_reuse_entry_hash(keys[e].unit, keys[e].set, keys[e].from) & (next - 1);
                    while (grown[to]) to = (to + 1) & (next - 1);
                    grown[to] = e + 1;
                }
                free(slots); slots = grown; slot_capacity = next;
            }
            size_t to = sl_reuse_entry_hash(mark->stock, mark->set, mark->from) & (slot_capacity - 1), entry = SIZE_MAX;
            while (slots[to]) {
                const sl_reuse_entry_key *key = keys + slots[to] - 1;
                if (key->unit == mark->stock && key->set == mark->set && key->from == mark->from) { entry = slots[to] - 1; break; }
                to = (to + 1) & (slot_capacity - 1);
            }
            if (entry == SIZE_MAX) {
                if (!sr_grow((void **)&r->stock, &r->stock_capacity, r->stock_count, sizeof(*r->stock)) ||
                    !sr_grow((void **)&keys, &key_capacity, r->stock_count, sizeof(*keys))) goto done;
                sl_reuse_stock *st = r->stock + r->stock_count;
                memset(st, 0, sizeof(*st));
                if (!(st->family = sr_intern(r, mark->stock->unit->family)) || !(st->name = sr_intern(r, mark->stock->unit->name)) ||
                    !sl_reuse_digest_of(s, mark->stock, st->digest)) goto done;
                memcpy(st->semantics, mark->stock->unit->semantics, 32);
                /* A restore checks the stock unit's own current inputs (sl_reuse_judge): no paths are kept. */
                st->path_first = r->path_count; st->path_count = 0;
                keys[r->stock_count] = (sl_reuse_entry_key){mark->stock, mark->set, mark->from};
                entry = r->stock_count++;
                slots[to] = entry + 1;
            }
            if (!sr_grow((void **)&r->bindings, &r->binding_capacity, r->binding_count, sizeof(*r->bindings))) goto done;
            r->bindings[r->binding_count++] = (sl_reuse_binding){entry, mark->pass};
            rs->count++;
        }
    }
    s->reuse.counts.scopes_recorded = r->scope_count; s->reuse.counts.bindings_recorded = r->binding_count;
    s->reuse.counts.stock_recorded = r->stock_count;
    sh_package_source_reuse_move(s->reuse.out, r);
    ok = 1;
done:
    sh_package_source_reuse_close(r); free(order); free(starts); free(slots); free(members); free(keys);
    s->reuse.counts.record_us += sl_now_us() - began;
    return ok ? 1 : sl_env_fail(s, "cannot record the capture's scope reuse", NULL);
}
/* Portable record encoding (R-348): strings, stock entries, scopes and bindings, little-endian, every count and index
 * checked against what remains and the table it names before anything is kept. */
static void sl_bytes_u32(sl_bytes *b, uint32_t value)
{
    unsigned char v[4];
    for (int i = 0; i < 4; i++) v[i] = (unsigned char)(value >> (8 * i));
    sl_bytes_put(b, v, sizeof(v));
}
int sh_package_source_reuse_encode(const sh_package_source_reuse *r, unsigned char **bytes, size_t *length)
{
    sl_bytes b = {0};
    size_t *index = NULL;
    *bytes = NULL; *length = 0;
    if (!r) return 0;
    /* Each interned string's position: the stock entries name them by index. */
    if (r->string_capacity && !(index = calloc(r->string_capacity, sizeof(*index)))) return 0;
    /* Settled groups by key, each key once (the first recorded), in an order of their own (the record keeps its
     * order). */
    const sr_settled **settled = NULL;
    size_t settled_count = 0;
    if (r->settled_count) {
        if (!(settled = malloc(r->settled_count * sizeof(*settled)))) { free(index); return 0; }
        for (size_t i = 0; i < r->settled_count; i++) settled[i] = r->settled + i;
        qsort(settled, r->settled_count, sizeof(*settled), sr_settled_pointer_order);
        for (size_t i = 0; i < r->settled_count; i++)
            if (!settled_count || memcmp(settled[settled_count - 1]->key, settled[i]->key, 32)) settled[settled_count++] = settled[i];
    }
    sl_bytes_put(&b, "SMPSR2", 6);
    sl_bytes_u64(&b, r->string_count);
    for (size_t i = 0, n = 0; i < r->string_capacity; i++) if (r->strings[i]) {
        size_t len = strlen(r->strings[i]);
        index[i] = n++;
        sl_bytes_u32(&b, (uint32_t)len); sl_bytes_put(&b, r->strings[i], len);
    }
    sl_bytes_u64(&b, r->stock_count);
    for (size_t i = 0; i < r->stock_count; i++) {
        const sl_reuse_stock *st = r->stock + i;
        for (int part = 0; part < 2; part++) {
            const char *text = part ? st->name : st->family;
            size_t at = (size_t)sl_hash_text(0, text) & (r->string_capacity - 1);
            while (r->strings[at] != text) at = (at + 1) & (r->string_capacity - 1);
            sl_bytes_u32(&b, (uint32_t)index[at]);
        }
        sl_bytes_put(&b, st->digest, 32); sl_bytes_put(&b, st->semantics, 32);
    }
    sl_bytes_u64(&b, r->scope_count);
    for (size_t i = 0; i < r->scope_count; i++) {
        sl_bytes_put(&b, r->scopes[i].key, 32); sl_bytes_u64(&b, r->scopes[i].first); sl_bytes_u64(&b, r->scopes[i].count);
    }
    sl_bytes_u64(&b, r->binding_count);
    for (size_t i = 0; i < r->binding_count; i++) {
        sl_bytes_u32(&b, (uint32_t)r->bindings[i].stock); unsigned char pass = (unsigned char)r->bindings[i].pass;
        sl_bytes_put(&b, &pass, 1);
    }
    sl_bytes_u64(&b, settled_count);
    for (size_t i = 0; i < settled_count; i++) {
        sl_bytes_put(&b, settled[i]->key, 32); sl_bytes_u64(&b, settled[i]->length);
        if (settled[i]->length) sl_bytes_put(&b, settled[i]->outputs, settled[i]->length);
    }
    free((void *)settled);
    free(index);
    if (b.failed || r->string_count > UINT32_MAX || r->stock_count > UINT32_MAX) { free(b.bytes); return 0; }
    *bytes = b.bytes; *length = b.length;
    return 1;
}
typedef struct sl_cursor { const unsigned char *at, *end; } sl_cursor;
static int sl_cursor_take(sl_cursor *c, void *out, size_t length)
{
    if ((size_t)(c->end - c->at) < length) return 0;
    memcpy(out, c->at, length); c->at += length; return 1;
}
static int sl_cursor_u64(sl_cursor *c, uint64_t *out)
{
    unsigned char v[8];
    if (!sl_cursor_take(c, v, 8)) return 0;
    *out = 0;
    for (int i = 7; i >= 0; i--) *out = *out << 8 | v[i];
    return 1;
}
static int sl_cursor_u32(sl_cursor *c, uint32_t *out)
{
    unsigned char v[4];
    if (!sl_cursor_take(c, v, 4)) return 0;
    *out = (uint32_t)v[0] | (uint32_t)v[1] << 8 | (uint32_t)v[2] << 16 | (uint32_t)v[3] << 24;
    return 1;
}
/* A count of elements of at least element bytes each that fit in what remains. */
static int sl_cursor_count(sl_cursor *c, size_t element, size_t *out)
{
    uint64_t n;
    if (!sl_cursor_u64(c, &n) || n > (uint64_t)(c->end - c->at) / element) return 0;
    *out = (size_t)n; return 1;
}
int sh_package_source_reuse_decode(const unsigned char *bytes, size_t length, sh_package_source_reuse **out)
{
    sl_cursor c = {bytes, bytes + length};
    sh_package_source_reuse *r = sh_package_source_reuse_open();
    const char **strings = NULL;
    char magic[6];
    size_t count = 0;
    int result = 0;
    *out = NULL;
    if (!r) return -1;
    /* SMPSR1 (before settled groups) still reads, with none. */
    int settled = bytes && length >= 6 && !memcmp(bytes, "SMPSR2", 6);
    if (!bytes || !sl_cursor_take(&c, magic, 6) || (memcmp(magic, "SMPSR1", 6) && memcmp(magic, "SMPSR2", 6)) ||
        !sl_cursor_count(&c, 4, &count)) goto done;
    if (count && !(strings = calloc(count, sizeof(*strings)))) { result = -1; goto done; }
    for (size_t i = 0; i < count; i++) {
        uint32_t len;
        char *text;
        if (!sl_cursor_u32(&c, &len) || len > (size_t)(c.end - c.at)) goto done;
        if (!(text = malloc((size_t)len + 1))) { result = -1; goto done; }
        memcpy(text, c.at, len); text[len] = 0; c.at += len;
        if (strlen(text) != len) { free(text); goto done; }
        strings[i] = sr_intern(r, text); free(text);
        if (!strings[i]) { result = -1; goto done; }
    }
    size_t stock_count;
    if (!sl_cursor_count(&c, 72, &stock_count)) goto done;
    if (stock_count && !(r->stock = calloc(stock_count, sizeof(*r->stock)))) { result = -1; goto done; }
    r->stock_capacity = stock_count;
    for (size_t i = 0; i < stock_count; i++) {
        uint32_t family, name;
        sl_reuse_stock *st = r->stock + i;
        if (!sl_cursor_u32(&c, &family) || !sl_cursor_u32(&c, &name) || family >= count || name >= count ||
            !sl_cursor_take(&c, st->digest, 32) || !sl_cursor_take(&c, st->semantics, 32)) goto done;
        st->family = strings[family]; st->name = strings[name];
        r->stock_count = i + 1;
    }
    size_t scope_count, binding_count;
    if (!sl_cursor_count(&c, 48, &scope_count)) goto done;
    if (scope_count && !(r->scopes = calloc(scope_count, sizeof(*r->scopes)))) { result = -1; goto done; }
    r->scope_capacity = scope_count;
    for (size_t i = 0; i < scope_count; i++) {
        uint64_t first, n;
        if (!sl_cursor_take(&c, r->scopes[i].key, 32) || !sl_cursor_u64(&c, &first) || !sl_cursor_u64(&c, &n)) goto done;
        r->scopes[i].first = (size_t)first; r->scopes[i].count = (size_t)n;
        if (first > SIZE_MAX || n > SIZE_MAX) goto done;
        r->scope_count = i + 1;
    }
    if (!sl_cursor_count(&c, 5, &binding_count)) goto done;
    if (binding_count && !(r->bindings = calloc(binding_count, sizeof(*r->bindings)))) { result = -1; goto done; }
    r->binding_capacity = binding_count;
    for (size_t i = 0; i < binding_count; i++) {
        uint32_t stock; unsigned char pass;
        if (!sl_cursor_u32(&c, &stock) || !sl_cursor_take(&c, &pass, 1) || stock >= r->stock_count || pass < 1 || pass > 3)
            goto done;
        r->bindings[i].stock = stock; r->bindings[i].pass = pass;
        r->binding_count = i + 1;
    }
    for (size_t i = 0; i < r->scope_count; i++)
        if (r->scopes[i].first > r->binding_count || r->scopes[i].count > r->binding_count - r->scopes[i].first) goto done;
    if (settled) {
        size_t settled_count;
        if (!sl_cursor_count(&c, 40, &settled_count)) goto done;
        if (settled_count && !(r->settled = calloc(settled_count, sizeof(*r->settled)))) { result = -1; goto done; }
        r->settled_capacity = settled_count;
        for (size_t i = 0; i < settled_count; i++) {
            sr_settled *entry = r->settled + i;
            uint64_t size;
            if (!sl_cursor_take(&c, entry->key, 32) || !sl_cursor_u64(&c, &size) ||
                size > (uint64_t)(c.end - c.at) || (i && memcmp(r->settled[i - 1].key, entry->key, 32) >= 0)) goto done;
            if (size && !(entry->outputs = malloc((size_t)size))) { result = -1; goto done; }
            if (size) { memcpy(entry->outputs, c.at, (size_t)size); c.at += size; }
            entry->length = (size_t)size;
            r->settled_count = r->settled_sorted = i + 1;
        }
    }
    if (c.at != c.end) goto done;
    *out = r; r = NULL; result = 1;
done:
    free(strings); sh_package_source_reuse_close(r);
    return result;
}
/* At open: every package scope with folds is keyed when a record is kept or offered; offered ones match by key. */
static int sl_reuse_open(sh_package_source_link *s, const sh_package_source_root_plan *plan)
{
    s->reuse.table_scope = SIZE_MAX;
    if (!s->reuse.out && !s->reuse.in) return 1;
    if (s->share.mode != 1) { s->reuse.out = NULL; s->reuse.in = NULL; return 1; }
    unsigned long long began = sl_now_us();
    s->reuse.keys = calloc(s->scope_count ? s->scope_count : 1, sizeof(*s->reuse.keys));
    s->reuse.keyed = calloc(s->scope_count ? s->scope_count : 1, 1);
    s->reuse.matched = malloc((s->scope_count ? s->scope_count : 1) * sizeof(*s->reuse.matched));
    if (!s->reuse.keys || !s->reuse.keyed || !s->reuse.matched) return sl_env_fail(s, "cannot key scopes for reuse", NULL);
    unsigned long long before[4], after[4];
    int metered = sh_package_memory_read(before);
    for (size_t scope = 0; scope < s->scope_count; scope++) {
        s->reuse.matched[scope] = SIZE_MAX;
        if (scope == s->share.baseline || !sl_share_scope(s, scope)) continue;
        if (!sl_reuse_scope_key(s, scope, plan, s->reuse.keys[scope]))
            return sl_env_fail(s, "cannot key a scope for reuse", NULL);
        s->reuse.keyed[scope] = 1; s->reuse.counts.scopes_keyed++;
        size_t index;
        if (s->reuse.in && sr_scope_of(s->reuse.in, s->reuse.keys[scope], &index)) {
            s->reuse.matched[scope] = index; s->reuse.counts.scopes_matched++;
            if (s->native.reuse_attempted) *s->native.reuse_attempted = 1;
        }
    }
    s->reuse.counts.scopes_offered = s->reuse.in ? s->reuse.in->scope_count : 0;
    s->reuse.counts.key_us += sl_now_us() - began;
    if (metered && sh_package_memory_read(after)) s->reuse.counts.key_private_mb = (long long)after[0] - (long long)before[0];
    if (s->reuse.in) s->reuse.counts.offered_bytes = sh_package_source_reuse_bytes(s->reuse.in);
    return 1;
}
static void sl_reuse_free(sh_package_source_link *s)
{
    free(s->reuse.keys); free(s->reuse.keyed); free(s->reuse.matched); free(s->reuse.sets); free(s->reuse.set_slots);
    free(s->reuse.paths); free(s->reuse.path_marks); free(s->reuse.marks); free(s->reuse.candidates);
    free(s->reuse.candidate_slots); free(s->reuse.digests);
}
/* The freeze: what the capture kept to judge, restore and record bindings (its size counted) goes; the counters and
 * the records stay. */
static void sl_reuse_release(sh_package_source_link *s)
{
    s->reuse.counts.mark_bytes = s->reuse.mark_capacity * sizeof(*s->reuse.marks);
    s->reuse.counts.working_bytes = s->reuse.counts.mark_bytes + s->reuse.set_capacity * sizeof(*s->reuse.sets) +
        s->reuse.set_slot_capacity * sizeof(*s->reuse.set_slots) + s->reuse.path_capacity * sizeof(*s->reuse.paths) +
        s->reuse.path_mark_capacity +
        s->reuse.candidate_capacity * sizeof(*s->reuse.candidates) +
        s->reuse.candidate_slot_capacity * sizeof(*s->reuse.candidate_slots) +
        s->reuse.digest_capacity * sizeof(*s->reuse.digests);
    if (s->reuse.out) s->reuse.counts.record_bytes = sh_package_source_reuse_bytes(s->reuse.out);
    sl_reuse_free(s);
    s->reuse.keys = NULL; s->reuse.keyed = NULL; s->reuse.matched = NULL;
    s->reuse.sets = NULL; s->reuse.set_count = s->reuse.set_capacity = 0;
    s->reuse.set_slots = NULL; s->reuse.set_slot_capacity = 0;
    s->reuse.paths = NULL; s->reuse.path_count = s->reuse.path_capacity = 0;
    s->reuse.path_marks = NULL; s->reuse.path_mark_capacity = 0;
    s->reuse.marks = NULL; s->reuse.mark_count = s->reuse.mark_capacity = 0;
    s->reuse.candidates = NULL; s->reuse.candidate_count = s->reuse.candidate_capacity = 0;
    s->reuse.candidate_slots = NULL; s->reuse.candidate_slot_capacity = 0; s->reuse.table_scope = SIZE_MAX;
    s->reuse.digests = NULL; s->reuse.digest_count = s->reuse.digest_capacity = 0;
}
#if defined(SH_PACKAGE_LIBRARY_TESTING) || defined(SH_PACKAGE_SOURCE_LINK_TESTING)
/* Tests: offer scope `to` of a record the binding scope `from` records for family and name too (a contradiction a
 * capture must refuse). 0 when there is none, or on allocation failure. */
int sh_package_source_reuse_test_copy_binding(sh_package_source_reuse *r, size_t from, const char *family,
    const char *name, size_t to, unsigned pass)
{
    if (!r || from >= r->scope_count || to >= r->scope_count) return 0;
    const sl_reuse_scope *source = r->scopes + from;
    size_t found = SIZE_MAX;
    for (size_t i = 0; i < source->count && found == SIZE_MAX; i++) {
        const sl_reuse_stock *st = r->stock + r->bindings[source->first + i].stock;
        if (!strcmp(st->family, family) && !strcmp(st->name, name)) found = source->first + i;
    }
    if (found == SIZE_MAX || !sr_grow((void **)&r->bindings, &r->binding_capacity, r->binding_count, sizeof(*r->bindings)))
        return 0;
    sl_reuse_binding copy = r->bindings[found];
    if (pass) copy.pass = pass;
    size_t at = r->scopes[to].first + r->scopes[to].count;
    memmove(r->bindings + at + 1, r->bindings + at, (r->binding_count - at) * sizeof(*r->bindings));
    r->bindings[at] = copy; r->binding_count++; r->scopes[to].count++;
    for (size_t k = 0; k < r->scope_count; k++) if (k != to && r->scopes[k].first >= at) r->scopes[k].first++;
    return 1;
}
#endif
#if defined(SH_PACKAGE_LIBRARY_TESTING) || defined(SH_PACKAGE_SOURCE_LINK_TESTING)
/* Tests: the i-th settled entry in the record's order (a decoded record's is by key), and its outputs' length. */
int sh_package_source_reuse_test_settled_entry(const sh_package_source_reuse *r, size_t i, unsigned char key[32],
    size_t *length)
{
    if (!r || !key || i >= r->settled_count) return 0;
    memcpy(key, r->settled[i].key, 32);
    if (length) *length = r->settled[i].length;
    return 1;
}
/* Tests: a record as text, one scope per block (its key prefix), one binding per line. malloc-owned. */
char *sh_package_source_reuse_test_text(const sh_package_source_reuse *r)
{
    sl_bytes b = {0};
    char line[1024];
    for (size_t k = 0; r && k < r->scope_count; k++) {
        const sl_reuse_scope *rs = r->scopes + k;
        int n = snprintf(line, sizeof(line), "scope %zu key %02x%02x%02x%02x bindings %zu", k, rs->key[0], rs->key[1],
            rs->key[2], rs->key[3], rs->count);
        sl_bytes_put(&b, line, (size_t)n); sl_bytes_put(&b, "\n", 1);
        for (size_t i = 0; i < rs->count; i++) {
            const sl_reuse_binding *binding = r->bindings + rs->first + i;
            const sl_reuse_stock *st = r->stock + binding->stock;
            n = snprintf(line, sizeof(line), "  pass %u %s %s digest %02x%02x%02x%02x paths %zu", binding->pass, st->family,
                st->name, st->digest[0], st->digest[1], st->digest[2], st->digest[3], st->path_count);
            sl_bytes_put(&b, line, (size_t)n); sl_bytes_put(&b, "\n", 1);
        }
    }
    sl_bytes_put(&b, "", 1);
    return b.failed ? (free(b.bytes), NULL) : (char *)b.bytes;
}
#endif
void sh_package_source_link_reuse_counters(const sh_package_source_link *s, sh_package_source_reuse_counters *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (s) *out = s->reuse.counts;
}
#if defined(SH_PACKAGE_LIBRARY_TESTING) || defined(SH_PACKAGE_SOURCE_LINK_TESTING) ||     defined(SH_RESOURCE_GRAPH_TESTING) || defined(SH_PACKAGE_RUNTIME_TESTING)
/* Tests: the occurrences of shared stock each frozen capture bound (mode 2: those it would bind), as lines
 * "<package name, or #scope>\t<family>\t<name>" appended since the log was last enabled. */
static int g_sl_shared_log_on;
static char *g_sl_shared_log;
static size_t g_sl_shared_log_length;
void sh_package_source_link_test_shared_log(int enabled)
{
    free(g_sl_shared_log); g_sl_shared_log = NULL; g_sl_shared_log_length = 0; g_sl_shared_log_on = enabled != 0;
}
const char *sh_package_source_link_test_shared_logged(void) { return g_sl_shared_log ? g_sl_shared_log : ""; }
static void sl_shared_log_line(const sh_package_source_link *s, size_t scope, const sh_package_link_unit *unit)
{
    const sh_package_sources *sources = s->scopes[scope] ? s->scopes[scope]->sources : NULL;
    char number[32];
    const char *label = number;
    snprintf(number, sizeof(number), "#%zu", scope);
    for (size_t i = 0; sources && i < sources->component_count; i++)
        if (!*sources->components[i].relative) { label = sources->components[i].descriptor.name; break; }
    size_t add = strlen(label) + strlen(unit->family) + strlen(unit->name) + 3;
    char *grown = realloc(g_sl_shared_log, g_sl_shared_log_length + add + 1);
    if (!grown) return;
    g_sl_shared_log = grown;
    snprintf(grown + g_sl_shared_log_length, add + 1, "%s\t%s\t%s\n", label, unit->family, unit->name);
    g_sl_shared_log_length += add;
}
static void sl_shared_log(const sh_package_source_link *s)
{
    if (!g_sl_shared_log_on) return;
    for (size_t scope = 0; s->share.maps && scope < s->scope_count; scope++)
        for (size_t j = 0; j < s->share.maps[scope].count; j++) sl_shared_log_line(s, scope, s->share.maps[scope].units[j]->unit);
    for (size_t i = 0; s->share.mode == 2 && i < s->count; i++)
        if (s->units[i]->scope != s->share.baseline && sl_rep_of(s, s->units[i]))
            sl_shared_log_line(s, s->units[i]->scope, s->units[i]->unit);
}
#define SL_SHARED_LOG(s) sl_shared_log(s)
/* Tests (R-348): the canonical text of each frozen capture, appended while enabled, to compare a restored capture
 * with a fresh one exactly. Every unit in its frozen order with its scope, identity, files and witnesses by path and
 * resolution, discovered files, site, implicit and conditional edges by unit index, certificate and flags (not the
 * diagnostics of how it was discovered), then each scope's share map by unit index. */
static int g_sl_capture_dump_on;
static char *g_sl_capture_dump;
static size_t g_sl_capture_dump_length, g_sl_capture_dump_capacity;
void sh_package_source_link_test_capture_dump(int enabled)
{
    free(g_sl_capture_dump); g_sl_capture_dump = NULL; g_sl_capture_dump_length = g_sl_capture_dump_capacity = 0;
    g_sl_capture_dump_on = enabled != 0;
}
const char *sh_package_source_link_test_capture_dumped(void) { return g_sl_capture_dump ? g_sl_capture_dump : ""; }
static void sl_dump_put(const char *format, ...)
{
    va_list args;
    for (;;) {
        size_t room = g_sl_capture_dump_capacity - g_sl_capture_dump_length;
        va_start(args, format);
        int n = g_sl_capture_dump_capacity ? vsnprintf(g_sl_capture_dump + g_sl_capture_dump_length, room, format, args) : -1;
        va_end(args);
        if (n >= 0 && (size_t)n < room) { g_sl_capture_dump_length += (size_t)n; return; }
        size_t next = g_sl_capture_dump_capacity ? g_sl_capture_dump_capacity * 2 : 65536;
        char *grown = realloc(g_sl_capture_dump, next);
        if (!grown) { g_sl_capture_dump_on = 0; return; }
        g_sl_capture_dump = grown; g_sl_capture_dump_capacity = next;
    }
}
static void sl_capture_dump(const sh_package_source_link *s)
{
    if (!g_sl_capture_dump_on) return;
    sl_dump_put("capture %zu units %zu scopes\n", s->count, s->scope_count);
    for (size_t i = 0; i < s->count && g_sl_capture_dump_on; i++) {
        const sl_unit *unit = s->units[i];
        const sh_package_link_unit *r = unit->unit;
        sl_dump_put("u%zu s%u %s %s f", i, (unsigned)unit->scope, r->family, r->name);
        for (size_t f = 0; f < r->file_count; f++)
            sl_dump_put(" %s:%llu", r->files[f]->engine_path, (unsigned long long)sl_reuse_kind(s, unit, r->files[f]));
        sl_dump_put(" w");
        for (size_t w = 0; w < r->witness_count; w++)
            sl_dump_put(" %s=%s:%llu", r->witnesses[w].role, r->witnesses[w].resource->engine_path,
                (unsigned long long)sl_reuse_kind(s, unit, r->witnesses[w].resource));
        sl_dump_put(" d");
        for (size_t k = 0; k < unit->discovered_count; k++) sl_dump_put(" %s", unit->discovered[k]->engine_path);
        sl_dump_put(" e");
        for (size_t k = 0; k < unit->site_count; k++) sl_dump_put(" %s>%zu", unit->site_edges[k].slot, unit->site_edges[k].target);
        sl_dump_put(" i");
        for (size_t k = 0; k < r->implicit_count; k++) sl_dump_put(" %s>%zu", r->implicit[k].slot, r->implicit[k].target);
        sl_dump_put(" c");
        for (size_t k = 0; k < r->conditional_count; k++) sl_dump_put(" %s>%zu", r->conditional[k].slot, r->conditional[k].target);
        sl_dump_put(" sem");
        for (size_t k = 0; k < 32; k++) sl_dump_put("%02x", r->semantics[k]);
        sl_dump_put(" ob%d st%d so%d fc%d cv%d cb%d pr%d ar%d al%d sh%d own%d/%d fsm=%s\n", r->original_binding,
            unit->shared_stock, unit->stock_only, unit->fold_certified, unit->coverage, unit->campaign_boundary,
            unit->presentation, unit->authored_request, unit->authored_layer, s->shared ? s->shared[i] : -1,
            r->implicit_owners ? (int)sh_package_owners_count(r->implicit_owners) : -1,
            r->implicit_gameplay_owners ? (int)sh_package_owners_count(r->implicit_gameplay_owners) : -1,
            SL_EXTRA(unit)->fsm_config ? SL_EXTRA(unit)->fsm_config : "");
    }
    for (size_t k = 0; s->share.maps && k < s->scope_count && g_sl_capture_dump_on; k++) {
        if (!s->share.maps[k].count) continue;
        sl_dump_put("m%zu", k);
        for (size_t j = 0; j < s->share.maps[k].count; j++) sl_dump_put(" %u", (unsigned)s->share.maps[k].units[j]->index);
        sl_dump_put("\n");
    }
}
#define SL_CAPTURE_DUMP(s) sl_capture_dump(s)
#else
#define SL_SHARED_LOG(s) ((void)0)
#define SL_CAPTURE_DUMP(s) ((void)0)
#endif
/* Discover every scope's pending units scope by scope, the baseline first, folding each package scope's proven stock
 * copies after its pass (sl_fold_scope). Stops as sl_discover_until does. Within a scope the order is its units'
 * creation order, as without sharing. */
static int sl_discover_scopes(sh_package_source_link *s)
{
    size_t baseline = s->share.baseline;
    s->reuse.pass++;
    for (size_t step = 0; step < s->scope_count; step++) {
        size_t scope = !step ? baseline : step - 1 < baseline ? step - 1 : step;
        sl_pending *pending = s->share.pending + scope;
        int folding = sl_share_scope(s, scope) && scope != baseline;
        s->share.batch_count = 0;
        /* A restored scope is offered its record's bindings of this pass first (R-348). */
        int restoring = folding && sl_reuse_restoring(s, scope);
        if (restoring && !sl_reuse_begin(s, scope)) { sl_reuse_end(s, scope); pending->head = pending->count = 0; return 0; }
        while (pending->head < pending->count) {
            sl_unit *unit = pending->units[pending->head++];
            if (!unit) continue; /* an admission copy a bound stock unit replaced */
            if (!sl_discover_one(s, unit)) { sl_reuse_end(s, scope); pending->head = pending->count = 0; return 0; }
            if (folding && unit->discovery->discovered_done && !(s->withdrawn && s->withdrawn[scope]) &&
                !sl_units_push(&s->share.batch, &s->share.batch_count, &s->share.batch_capacity, unit))
                return sl_env_fail(s, "cannot retain shared stock candidates", unit->unit->name);
        }
        free(pending->units); pending->units = NULL; pending->head = pending->count = pending->capacity = 0;
        if (restoring) sl_reuse_end(s, scope);
        if (folding && !sl_fold_scope(s, scope)) return 0;
    }
    return 1;
}
/* From admission on, units wait on their scope's pending list (sl_find queues new ones). */
static int sl_pending_open(sh_package_source_link *s, size_t from)
{
    if (!(s->share.pending = calloc(s->scope_count ? s->scope_count : 1, sizeof(*s->share.pending))))
        return sl_env_fail(s, "cannot queue scoped resource identities", NULL);
    for (size_t i = 0; i < from; i++) {
        sl_unit *unit = s->units[i];
        sl_pending *pending = s->share.pending + unit->scope;
        if (!unit->discovery->discovered_done || !sl_share_scope(s, unit->scope)) continue;
        if (!sl_units_push(&pending->admitted, &pending->admitted_count, &pending->admitted_capacity, unit))
            return sl_env_fail(s, "cannot queue scoped resource identities", unit->unit->name);
    }
    for (size_t i = from; i < s->count; i++) {
        sl_unit *unit = s->units[i];
        sl_pending *pending = s->share.pending + unit->scope;
        if (!sl_units_push(&pending->units, &pending->count, &pending->capacity, unit))
            return sl_env_fail(s, "cannot queue scoped resource identities", unit->unit->name);
    }
    s->share.scope_major = 1;
    return 1;
}
void sh_package_source_link_close(sh_package_source_link *s)
{
    if (!s) return;
    if (s->rebuilt) { sl_rebuilt_free(s); return; } /* a stock reverse base rebuilt from a retained graph owns only its blocks */
    for (size_t i=0;i<s->module_row_count;i++) free((char *)s->module_rows[i].module); free(s->module_rows);
    for (size_t i = 0; i < s->lookup.capacity; i++) free(s->lookup.slots[i].key);
    free(s->lookup.slots);
    sl_store_clear(&s->own);
    if (s->memo) _InterlockedExchange(&s->memo->busy, 0);
    for (size_t i = 0; s->readers && i < s->scope_count; i++) sh_package_source_reader_close(s->readers[i]);
    free(s->canonical_marks);
    for (size_t i = 0; i < s->count; i++) {
        sl_unit *unit = s->units[i];
        if (!unit) continue; /* freed by shared stock before a failure (sl_units_compact) */
        sl_unit_parts_free(unit);
#if defined(SH_PACKAGE_LIBRARY_TESTING) || defined(SH_PACKAGE_SOURCE_LINK_TESTING)
        if (s->frozen_rows && unit->preserved && unit->shared_stock && s->table && i < s->table_capacity) {
            /* A shared stock row binds every scope that shares it: only its class-wide fixed and stable names may
             * change (the library falls back to the scope's own copies for anything else). */
            sh_package_link_unit edited = s->table[i];
            edited.fixed_name = s->frozen_rows[i].fixed_name; edited.stable_name = s->frozen_rows[i].stable_name;
            g_sl_shared_rows_checked++;
            if (!sl_row_same(&edited, s->frozen_rows + i)) {
                g_sl_shared_rows_changed++;
                if (g_sl_rows_strict) {
                    fprintf(stderr, "shared stock row %zu (%s %s) changed for one scope\n", i, unit->unit->family,
                        unit->unit->name);
                    abort();
                }
            }
        }
        if (s->frozen_rows && !unit->preserved) {
            g_sl_rows_checked++;
            if (!sl_row_same(unit->unit, s->frozen_rows + i)) {
                g_sl_rows_changed++;
                if (g_sl_rows_strict) {
                    fprintf(stderr, "captured row %zu (%s %s) changed without a kept copy\n", i, unit->unit->family,
                        unit->unit->name);
                    abort();
                }
            }
        }
#endif
        if (unit->preserved) free(unit->unit);
        free(unit);
    }
    sl_arena_free(&s->dependency_arena); sl_arena_free(&s->site_arena); sl_arena_free(&s->discovery_arena);
    free(s->site_edge_block); free(s->site_run_block);
    for (size_t i = 0; i < s->string_capacity; i++) free(s->strings[i]);
    free(s->strings);
    for (size_t i = 0; i < s->original_count; i++) {
        sl_product_clear(&s->originals[i]->resource);
        free((void *)s->originals[i]->resource.engine_path);
        if (s->originals[i]->resource.body_owner)
            sh_package_body_drop(s->originals[i]->resource.body_owner, s->originals[i]->resource.body);
        else if (!s->originals[i]->borrowed) free(s->originals[i]->resource.body);
        free(s->originals[i]);
    }
    for (size_t i = 0; i < s->scope_product_count; i++) {
        sh_compiled_resource *resource = s->scope_products[i].resource;
        sl_product_clear(resource);
        sh_package_owners_free(&resource->owners); sh_package_owners_free(&resource->gameplay_owners);
        free((char *)resource->engine_path); free(resource->body); free(resource);
    }
    free(s->scope_products);
    for (size_t i = 0; i < s->reverse_gap_count; i++) {
        free((char *)s->reverse_gaps[i].family); free((char *)s->reverse_gaps[i].name);
        free((char *)s->reverse_gaps[i].reason);
    }
    free(s->reverse_gaps); free(s->unpublished);
    for (size_t i = 0; i < s->player_hand_count; i++) free(s->player_hands[i]);
    free(s->player_hands);
    for (size_t i = 0; i < s->fsm_name_count; i++) free(s->fsm_names[i]);
    free(s->fsm_names);
    for (size_t i = 0; i < s->fsm_mask_count; i++) free(s->fsm_masks[i].config);
    free(s->fsm_masks);
    free(s->withdrawn);
    for (size_t i = 0; s->share.maps && i < s->scope_count; i++) free(s->share.maps[i].units);
    for (size_t i = 0; s->share.pending && i < s->scope_count; i++) {
        free(s->share.pending[i].units); free(s->share.pending[i].admitted);
    }
    free(s->share.maps); free(s->share.pending); free(s->share.batch); free(s->share.facts);
    free(s->share.reps); free(s->share.rep_names); free(s->share.fold); free(s->share.fsm_inputs);
    sl_reuse_free(s);
#if defined(SH_PACKAGE_LIBRARY_TESTING) || defined(SH_PACKAGE_SOURCE_LINK_TESTING) || \
    defined(SH_RESOURCE_GRAPH_TESTING) || defined(SH_PACKAGE_RUNTIME_TESTING)
    if (!s->observe) SL_SHARE_TOTALS(s);
#endif
    for (size_t i = 0; i < s->recovered_count; i++) free(s->recovered[i]);
    free(s->recovered);
    free(s->originals); free(s->original_slots); free(s->units); free(s->unit_slots);
    free(s->table ? s->table : s->inputs.units);
#if defined(SH_PACKAGE_LIBRARY_TESTING) || defined(SH_PACKAGE_SOURCE_LINK_TESTING)
    free(s->frozen_rows);
#endif
    free(s->inputs.scopes); free(s->readers); free(s->scopes); free(s->shared); free(s);
}
int sh_package_source_link_player_hands(const sh_package_source_link *s, const char *name)
{
    for (size_t i = 0; s && name && i < s->player_hand_count; i++) if (!strcmp(s->player_hands[i], name)) return 1;
    return 0;
}
size_t sh_package_source_link_reverse_gaps(const sh_package_source_link *source,
    const sh_package_reverse_gap **gaps)
{
    if (gaps) *gaps = source ? source->reverse_gaps : NULL;
    return source ? source->reverse_gap_count : 0;
}
size_t sh_package_source_link_shader_unpublished(const sh_package_source_link *source,
    const sh_package_shader_unpublished **records, int *lost)
{
    if (records) *records = source ? source->unpublished : NULL;
    if (lost) *lost = source ? source->unpublished_lost : 0;
    return source ? source->unpublished_count : 0;
}
size_t sh_package_source_link_recovered(const sh_package_source_link *source, const char *const **names)
{
    if (names) *names = source ? (const char *const *)source->recovered : NULL;
    return source ? source->recovered_count : 0;
}
/* Fixed native ABI identities (renderparm, renderprog, sampler) keep their original bindings
 * (package_native_names.c), and so must every unit they bind, or the linker's "a dependency keeps
 * no original name" rule would require renaming them. Mark every unit a fixed unit of the same
 * scope binds, through the edges the linker certifies: dependencies, implicit and conditional
 * bindings, and reference sites. 0 on allocation failure. */
static int sl_fixed_abi(const sl_unit *unit)
{
    return !strcmp(unit->unit->family, "renderparm") || !strcmp(unit->unit->family, "renderprog") ||
        !strcmp(unit->unit->family, "sampler");
}
static int sl_fixed_push(sl_unit ***stack, size_t *depth, size_t *room, sl_unit *unit)
{
    if (*depth == *room) {
        size_t next = *room ? *room * 2 : 64;
        sl_unit **grown = next <= SIZE_MAX / sizeof(*grown) ? realloc(*stack, next * sizeof(*grown)) : NULL;
        if (!grown) return 0;
        *stack = grown; *room = next;
    }
    (*stack)[(*depth)++] = unit;
    return 1;
}
static int sl_fixed_reach(sh_package_source_link *s)
{
    sl_unit **stack = NULL;
    size_t depth = 0, room = 0;
    for (size_t i = 0; i < s->count; i++) s->units[i]->discovery->fixed_reached = 0;
    for (size_t i = 0; i < s->count; i++) {
        sl_unit *root = s->units[i];
        if (!sl_fixed_abi(root) || !sl_fixed_push(&stack, &depth, &room, root)) {
            if (sl_fixed_abi(root)) { free(stack); return 0; }
            continue;
        }
        while (depth) {
            sl_unit *unit = stack[--depth];
            size_t total = unit->discovery->dependency_count + unit->discovery->implicit_count + unit->site_count;
            for (size_t e = 0; e < total; e++) {
                sl_unit *target = e < unit->discovery->dependency_count ? unit->discovery->dependencies[e] :
                    e < unit->discovery->dependency_count + unit->discovery->implicit_count ?
                        unit->discovery->implicit[e - unit->discovery->dependency_count].target :
                        unit->discovery->sites[e - unit->discovery->dependency_count - unit->discovery->implicit_count].target;
                if (!target || target->scope != root->scope || target->discovery->fixed_reached || sl_fixed_abi(target)) continue;
                target->discovery->fixed_reached = 1;
                if (!sl_fixed_push(&stack, &depth, &room, target)) { free(stack); return 0; }
            }
        }
    }
    free(stack);
    return 1;
}
static int sl_name_order(const void *a, const void *b)
{ return strcmp(*(char *const *)a, *(char *const *)b); }
/* Ask the availability callback for every file-backed image and decide per native name: a
 * placeholder some fixed unit of a gate-opening scope reaches is recovered in place (one native
 * object, one binding), every other placeholder is isolated in its scope. In a scope that does not
 * open the gate, an image the scope supplies or a supplied unit of it binds (its own state names it)
 * is asked too (availability_authored) and, when its native image is a placeholder, isolated the
 * same way unless a fixed unit of the scope reaches it, which keeps the stock binding (R-389).
 * 1 on success. */
static int sl_availability(sh_package_source_link *s, char *error, size_t capacity)
{
    if (!sl_fixed_reach(s)) {
        snprintf(error, capacity, "out of memory while marking fixed native ABI bindings"); return 0;
    }
    for (size_t i = 0; i < s->count; i++) s->units[i]->discovery->authored_image = 0;
    for (size_t i = 0; s->native.availability_authored && i < s->count; i++) {
        sl_unit *unit = s->units[i];
        /* The package's own files (or files composed with them), not product originals or inherited stock state. */
        if (!sl_unit_supplied(unit)) continue;
        if (!strcmp(unit->unit->family, "image")) unit->discovery->authored_image = 1;
        for (uint32_t e = 0; e < unit->discovery->dependency_count; e++) {
            sl_unit *target = unit->discovery->dependencies[e];
            if (!target || strcmp(target->unit->family, "image")) continue;
            if (target->scope == unit->scope) { target->discovery->authored_image = 1; continue; }
            /* A shared image this scope's own state binds: a fresh pass keeps the scope's own unit when its native
             * image is an exclusion placeholder (sl_fold_certificate), which a binding restored from a record kept
             * while it was usable cannot stand for. The compile fails, and is made again without reuse
             * (package_library.c); a fresh compile that still shares it fails rather than serve the placeholder. */
            unsigned char ignored[32];
            if (target->unit->file_count && sl_authored_excluded(s, unit->scope, target->unit->name, ignored)) {
                snprintf(error, capacity, "scope %u binds shared image %s, which its own state names and whose native "
                    "image is an exclusion placeholder", unit->scope, target->unit->name);
                s->failed_unit = unit; return 0;
            }
        }
    }
    for (size_t i = 0; i < s->count; i++) {
        sl_unit *unit = s->units[i];
        unit->discovery->availability = 0;
        free(unit->discovery->availability_semantics); unit->discovery->availability_semantics = NULL;
        if (!unit->unit->file_count || strcmp(unit->unit->family, "image")) continue;
        unsigned char semantics[32] = {0};
        int status = s->native.availability(s->native.context, unit->scope, unit->unit->name, semantics, error, capacity);
        if (status < 0 || status > 1) {
            if (error && capacity && !*error)
                snprintf(error, capacity, "native image availability could not be read: %s", unit->unit->name);
            s->failed_unit = unit; return 0;
        }
        if (!status && unit->discovery->authored_image && !unit->discovery->fixed_reached)
            status = sl_authored_excluded(s, unit->scope, unit->unit->name, semantics);
        if (status && !(unit->discovery->availability_semantics = malloc(32))) {
            snprintf(error, capacity, "cannot retain native image availability: %s", unit->unit->name); return 0;
        }
        if (status) memcpy(unit->discovery->availability_semantics, semantics, 32);
        unit->discovery->availability = (unsigned)status;
        if (!status || !unit->discovery->fixed_reached) continue;
        if (s->recovered_count == SIZE_MAX / sizeof(*s->recovered)) return 0;
        char **grown = realloc(s->recovered, (s->recovered_count + 1) * sizeof(*grown));
        char *copy = grown ? _strdup(unit->unit->name) : NULL;
        if (grown) s->recovered = grown;
        if (!copy) { snprintf(error, capacity, "out of memory while recording recovered images"); return 0; }
        s->recovered[s->recovered_count++] = copy;
    }
    if (s->recovered_count > 1) qsort(s->recovered, s->recovered_count, sizeof(*s->recovered), sl_name_order);
    size_t kept = 0;
    for (size_t i = 0; i < s->recovered_count; i++) {
        if (kept && !strcmp(s->recovered[kept - 1], s->recovered[i])) { free(s->recovered[i]); continue; }
        s->recovered[kept++] = s->recovered[i];
    }
    s->recovered_count = kept;
    return 1;
}
static int sl_recovered_has(const sh_package_source_link *s, const char *name)
{
    return s->recovered_count &&
        bsearch(&name, s->recovered, s->recovered_count, sizeof(*s->recovered), sl_name_order) != NULL;
}
sh_package_source_link *sh_package_source_link_open_attributed(const sh_package_compilation *const *scopes, size_t count,
    const sh_resource_catalog *catalog, sh_package_baseline_reader original, void *original_context,
    sh_decl_registry_source registry, uintptr_t reflection, const sh_package_source_link_native *native,
    const sh_package_source_root_plan *root_plan, sh_package_source_failure *failure,
    char *error, size_t capacity)
{
    SL_TEST_ENVIRONMENT();
    sh_package_source_link *s = calloc(1, sizeof(*s));
    sh_package_link_inputs roots = {0};
    char fallback[1024];
    unsigned long long mark = sl_now_ms();
    if (!error || !capacity) { error = fallback; capacity = sizeof(fallback); }
    *error = 0;
    if (failure) {
        failure->kind = SH_PACKAGE_SOURCE_FAILURE_NONE; failure->scope = SIZE_MAX;
        failure->family[0] = failure->name[0] = failure->chain[0] = 0;
    }
    if (!s) {
        if (failure) failure->kind = SH_PACKAGE_SOURCE_FAILURE_ENVIRONMENT;
        snprintf(error, capacity, "cannot allocate source linker"); return NULL;
    }
    s->failed_scope = SIZE_MAX;
    s->share.baseline = SIZE_MAX;
    s->lookup.limit = g_sl_lookup ? g_sl_lookup_limit : 0;
    s->error = error; s->error_capacity = capacity; s->catalog = catalog;
    s->original = original; s->original_context = original_context;
    if (native) {
        s->native = *native;
        if (native->shader) { s->shader = *native->shader; s->native.shader = &s->shader; }
    }
    if ((count && !scopes) || count > SIZE_MAX / sizeof(*s->scopes) || count > SIZE_MAX / sizeof(*s->readers)) {
        s->environment = 1; goto failed;
    }
    s->scopes = calloc(count ? count : 1, sizeof(*s->scopes));
    s->readers = calloc(count ? count : 1, sizeof(*s->readers));
    s->canonical_marks = calloc(count ? count : 1, 1);
    if (!s->scopes || !s->readers || !s->canonical_marks) { s->environment = 1; goto failed; }
    s->scope_count = count;
    for (size_t i = 0; i < count; i++) {
        /* Each distinct canonicalizer's place (marks name up to 255 of them; a scope beyond has none). */
        sl_canonicalizer own = sl_canonicalizer_of(scopes[i]);
        for (size_t j = 0; j < i && !s->canonical_marks[i]; j++)
            if (s->canonical_marks[j] && sl_canonicalizer_of(scopes[j]) == own) s->canonical_marks[i] = s->canonical_marks[j];
        if (!s->canonical_marks[i] && s->canonical_count < 255) s->canonical_marks[i] = (unsigned char)++s->canonical_count;
    }
    for (size_t i = 0; i < count; i++) {
        s->scopes[i] = scopes[i];
        /* Every scope reads the same native metadata: share the first reader's. */
        s->readers[i] = sh_package_source_reader_open_shared(i ? s->readers[0] : NULL, scopes[i], catalog,
            sl_read_original, s, registry, reflection, error, capacity);
        if (!s->readers[i]) { s->environment = 1; goto failed; }
        sh_package_source_reader_lookups(s->readers[i], sl_lookup, s);
        sh_package_source_reader_original_status(s->readers[i], SL_STATUS ? sl_original_status : NULL);
        if (s->native.shader && !sh_package_source_reader_shader(s->readers[i], s->native.shader)) {
            s->environment = 1; goto failed;
        }
    }
    /* Shared stock (R-344 stage A): the scopes whose proven stock copies may be bound once in the baseline. */
    if (root_plan && root_plan->fold && g_sl_share && root_plan->baseline_scope < count) {
        s->share.mode = g_sl_share; s->share.baseline = root_plan->baseline_scope;
        s->share.fold = calloc(count, 1); s->share.fsm_inputs = calloc(count, 1);
        s->share.maps = calloc(count, sizeof(*s->share.maps));
        if (!s->share.fold || !s->share.fsm_inputs || !s->share.maps) { s->environment = 1; goto failed; }
        for (size_t i = 0; i < count; i++) {
            /* A scope that keys paths with a canonicalizer of its own may resolve a stock lookup to another original
             * than the baseline would: it keeps its copies. */
            s->share.fold[i] = root_plan->fold[i] && i != s->share.baseline &&
                sl_canonicalizer_of(scopes[i]) == sl_canonicalizer_of(scopes[s->share.baseline]);
            /* A behavior's layer selection reads its scope's own state-machine declarations (sl_fsm_edges). */
            for (size_t f = 0; f < scopes[i]->resource_count && !s->share.fsm_inputs[i]; f++) {
                const char *type = scopes[i]->resources[f].type;
                s->share.fsm_inputs[i] = type && (!strcmp(type, "aifsmmanager") || !strcmp(type, "aibehavior"));
            }
        }
    }
    s->store = &s->own; s->capture = 1;
    if (s->native.memo && g_sl_replay && count) {
        sh_package_source_memo *memo = s->native.memo;
        unsigned char key[32];
        if (_InterlockedCompareExchange(&memo->busy, 1, 0)) s->census.memo = 3;
        else if (!sl_memo_key(s, registry, key)) {
            /* A capture that cannot establish the key records only for itself. */
            _InterlockedExchange(&memo->busy, 0);
            if (s->error && s->error_capacity) *s->error = 0;
        } else {
            s->census.memo = 1;
            if (!memo->keyed || memcmp(memo->key, key, sizeof(key))) {
                if (memo->keyed) s->census.memo = 2;
                sl_store_clear(&memo->store); memcpy(memo->key, key, sizeof(key)); memo->keyed = 1;
            }
            s->memo = memo; s->store = &memo->store; s->capture = ++memo->captures;
        }
    }
    /* Unchanged-scope reuse (R-348): key the package scopes, match an earlier record's. */
    s->reuse.out = s->native.reuse_out; s->reuse.in = s->native.reuse_in;
    if (!sl_reuse_open(s, root_plan)) { s->environment = 1; goto failed; }
    sl_phase(s, 0, &mark);
    if (!sl_seed_inputs(s)) goto failed;
    for (size_t i = 0; i < s->count; i++) { s->units[i]->discovery->seeded = 1; s->units[i]->discovery->seeded_witnesses = (uint32_t)s->units[i]->unit->witness_count; }
    sl_phase(s, 1, &mark);
    if (!sl_player_contract(s)) { s->environment = 1; goto failed; }
    /* Supplied content first: a supplied file its reader cannot read, or a
     * lookup that needs native preparation, fails here instead of after the
     * whole stock reverse discovery. The closure captured is the same. */
    size_t inspected = 0, seeds = s->count;
    if (!sl_seed_modules(s, seeds)) goto failed;
    /* Admission: every seed (the content the scopes supply, sl_seed_inputs)
     * is judged before any unit it requests. A seed failing on what its own
     * scope supplied dooms this capture, and the stock closure of the healthy
     * scopes would only be discovered again by the next one, so the capture
     * stops at the seed boundary, reporting every such failure together.
     * Pending native preparation keeps the whole frontier, and so does a
     * capture that can retain OpenGL source-cooking requests (the adapter's
     * program callback), which the closure reaches: one preparation round
     * should serve the next capture. */
    int cooking = s->native.shader && s->native.shader->renderer == SH_PACKAGE_SHADER_OPENGL &&
        s->native.shader->program;
    if (!sl_discover_until(s, &inspected, seeds)) goto failed;
    if (!cooking && s->deferred.unit && !*s->preparation_reason) {
        if (s->native.withdrawals) s->native.withdrawals->admission_units = seeds;
        (void)sl_phase_complete(s); goto failed;
    }
    /* With shared stock, scope by scope from here, each package scope's proven stock copies folded after its pass. */
    if (s->share.mode) {
        if (!sl_pending_open(s, inspected) || !sl_discover_scopes(s)) goto failed;
        sl_units_compact(s);
    } else if (!sl_discover_through(s, &inspected)) goto failed;
    if (s->deferred.unit || *s->preparation_reason) {
        /* This attempt fails. When native preparation is pending anyway, first
         * let the stock observation queue what it needs too, so one
         * preparation round serves the next attempt; its own outcome is not
         * what this attempt reports. With nothing to prepare, a failure stops
         * here, before any stock root is read. */
        if (*s->preparation_reason) {
            char kept[1024];
            snprintf(kept, sizeof(kept), "%s", s->error ? s->error : "");
            (void)sl_reverse_roots(s, root_plan, registry, reflection);
            s->failed = 0; s->environment = 0;
            if (s->error && s->error_capacity) snprintf(s->error, s->error_capacity, "%s", kept);
            sl_phase(s, 2, &mark);
        }
        (void)sl_phase_complete(s); goto failed;
    }
    sl_phase(s, 3, &mark);
    /* Stock reverse discovery refuses only for I/O, allocation or setup. */
    if (!sl_reverse_roots(s, root_plan, registry, reflection)) { s->environment = 1; goto failed; }
    sl_phase(s, 2, &mark);
    for (int pass = 0; pass < 2; pass++) {
        if (pass) sl_units_compact(s);
        if (pass && !sl_seed_orphan_binaries(s)) goto failed;
        if (!(s->share.mode ? sl_discover_scopes(s) : sl_discover_through(s, &inspected)) || !sl_phase_complete(s)) goto failed;
    }
    sl_units_compact(s);
    s->share.scope_major = 0;
    s->native.withdrawals = NULL; /* Borrowed for discovery only. */
    if (!sl_movie_late_requests(s)) goto failed;
    sl_phase(s, 3, &mark);
    if (!sl_shader_accounted(s)) goto failed;
    sl_phase(s, 4, &mark);
    if (s->native.audio && !sh_package_audio_program_prepare(s->native.audio, error, capacity)) {
        /* The capture of one scope's sounds and banks failed: that scope's
         * supplied audio is responsible, not its peers. */
        s->failed_scope = sh_package_audio_program_failed_scope(s->native.audio);
        goto failed;
    }
    sl_phase(s, 5, &mark);
    /* Complete explicit source closure before preparing potentially large
     * implicit renderer/audio data. A missing reference or malformed source
     * must fail before any expensive native-provider capture. */
    if (s->native.semantics && s->native.availability && !sl_availability(s, error, capacity)) goto failed;
    for (size_t i = 0; s->native.semantics && i < s->count; i++) {
        sl_unit *unit = s->units[i];
        /* A shared stock unit keeps the certificate its fold established in its scope and the baseline. */
        if (!unit->unit->file_count || unit->fold_certified) continue;
        int original_bytes = s->native.semantics(s->native.context, unit->scope, unit->unit->family,
            unit->unit->name, unit->unit->semantics, error, capacity);
        if (original_bytes < 0 || original_bytes > 1) { s->failed_unit = unit; goto failed; }
        /* A recovered image keeps its certified semantics: one native object serves every scope
         * through a fixed binding that cannot follow a private name, and the publication
         * recovers that object in place under the declared requirement (R-309). */
        if (unit->discovery->availability && !sl_recovered_has(s, unit->unit->name)) {
            memcpy(unit->unit->semantics, unit->discovery->availability_semantics, 32); original_bytes = 0;
            sl_note_original(unit, "its native image is an exclusion placeholder; served as a private stock copy");
        }
        if (!original_bytes) sl_note_original(unit, "native semantics are not original");
        int campaign = 0, certified = original_bytes, product = 0;
        for (size_t j = 0; j < unit->unit->file_count; j++) {
            /* Only a module family's member keeps the name its producer gives it
             * (produce_derived); other generated rows stay authored rows. */
            if (!unit->unit->files[j]->generated || !s->native.produce_derived ||
                s->native.produce_derived(s->native.producer_context, unit->unit->files[j]->engine_path) != 1) continue;
            sl_original *original = sl_original_get(s, unit->unit->files[j]->engine_path);
            if (!original || original->status < 0) { s->failed_unit = unit; goto failed; }
            product |= original->status == 3 && original->resource.generated;
        }
        for (size_t j = 0; original_bytes && j < unit->unit->file_count; j++) {
            sl_original *original = sl_original_get(s, unit->unit->files[j]->engine_path);
            size_t length = 0;
            if (!original || original->status < 0) goto failed;
            if (original->status != 1 && original->status != 2 && !(product && original->status == 3)) {
                char note[400];
                snprintf(note, sizeof(note), "original %s has status %d", unit->unit->files[j]->engine_path, original->status);
                sl_note_original(unit, note);
                original_bytes = 0; break;
            }
            campaign |= original->status == 2;
            const sh_compiled_resource *certificate = SL_EXTRA(unit)->unprepared ? unit->extra->unprepared : unit->unit->files[j];
            /* The captured original itself: its own bytes, nothing to compare. */
            if (certificate == &original->resource) continue;
            unsigned char *body = sh_package_compilation_read(unit->unit->compilation, certificate,
                SIZE_MAX, &length, error, capacity);
            if (!body) goto failed;
            unsigned char *owned = NULL;
            const unsigned char *stock = sl_original_bytes(s, original, &owned, NULL, 0);
            if (!stock) { free(body); goto failed; }
            original_bytes = original->resource.body_length == length && !memcmp(stock, body, length);
            if (!original_bytes && !SL_EXTRA(unit)->original_note) {
                /* The first differing offset and a sanitized window of both sides. */
                size_t at = 0, shorter = length < original->resource.body_length ? length : original->resource.body_length;
                while (at < shorter && body[at] == stock[at]) at++;
                char mine[49], theirs[49], note[512];
                size_t from = at > 16 ? at - 16 : 0;
                for (size_t k = 0; k < 48; k++) {
                    unsigned char a = from + k < length ? body[from + k] : ' ';
                    unsigned char o = from + k < original->resource.body_length ? stock[from + k] : ' ';
                    mine[k] = a >= 32 && a < 127 ? (char)a : '.'; theirs[k] = o >= 32 && o < 127 ? (char)o : '.';
                }
                mine[48] = theirs[48] = 0;
                snprintf(note, sizeof(note), "%s differs from its captured original (%zu vs %zu bytes, %s%s%s, %u sources, "
                    "first at %zu: [%s] vs [%s])", unit->unit->files[j]->engine_path, length, original->resource.body_length,
                    SL_EXTRA(unit)->unprepared ? "prepared" : "as bound", certificate->engine_path &&
                    strcmp(certificate->engine_path, unit->unit->files[j]->engine_path) ? " from " : "",
                    certificate->engine_path && strcmp(certificate->engine_path, unit->unit->files[j]->engine_path) ?
                    certificate->engine_path : "", (unsigned)certificate->source_count, at, mine, theirs);
                sl_note_original(unit, note);
            }
            free(owned); free(body);
        }
        /* Campaign-only bytes a scope supplies keep their native name (the
         * engine derives some names from others), but they are that scope's
         * content, not SnapMap's: another scope may rely on their absence (an
         * engine default), so a SnapMap unit reaching them is not original. */
        if (original_bytes && campaign) original_bytes = 2;
        else if (!original_bytes && certified && !strcmp(unit->unit->family, "renderprog")) {
            /* A render program's name is its native identity and cannot be
             * relocated yet. One SnapMap does not ship (a re-cooked campaign
             * program, say) keeps the name its scope gives it the same way. */
            char *path = sl_decl_path("renderprog", unit->unit->name);
            if (!path) { sl_env_fail(s, "cannot construct render program path", unit->unit->name); goto failed; }
            sl_original *declaration = sl_original_get(s, path);
            free(path);
            if (!declaration || declaration->status < 0) goto failed;
            if (declaration->status != 1 && declaration->status != 3) original_bytes = 2;
        }
        /* Its original is the product's own output (sl_product), which the
         * product serves under the name it generates it under; equal bytes
         * certify that binding as they would an installed original's. */
        unit->discovery->product = product;
        unit->unit->original_binding = original_bytes;
    }
    /* Original bindings are certified only with the native semantics adapter;
     * without it nothing here is known to be original or changed. */
    if (s->native.semantics && !sl_changed_bindings(s)) goto failed;
    /* Discovery and certification have read the originals they compare; the owners, inputs and every later
     * phase read backed originals only through their bodies (R-342). */
    sl_originals_release(s);
    sl_phase(s, 6, &mark);
    for (size_t i = 0; s->native.owners && i < s->count; i++) {
        sl_unit *unit = s->units[i];
        if (unit->unit->file_count && s->native.owners(s->native.context, unit->scope,
            unit->unit->family, unit->unit->name, &unit->unit->implicit_owners,
            &unit->unit->implicit_gameplay_owners, error, capacity) != 1) { s->failed_unit = unit; goto failed; }
    }
    sl_phase(s, 7, &mark);
    int shared_places = 0;
    for (size_t i = 0; s->share.maps && i < s->scope_count && !shared_places; i++) shared_places = s->share.maps[i].count != 0;
    if (shared_places && s->count > 1 && !SL_MUTATED(5)) {
        /* Before the sort indices are positions (sl_units_compact): the earliest scope binding each unit. */
        uint32_t *place = malloc(s->count * sizeof(*place));
        if (!place) { sl_env_fail(s, "cannot order shared stock", NULL); goto failed; }
        for (size_t i = 0; i < s->count; i++) place[i] = s->units[i]->stock_only ? UINT32_MAX : s->units[i]->scope;
        for (size_t k = 0; k < s->scope_count; k++)
            for (size_t j = 0; j < s->share.maps[k].count; j++) {
                uint32_t *at = place + s->share.maps[k].units[j]->index;
                if (k < *at) *at = (uint32_t)k;
            }
        qsort_s(s->units, s->count, sizeof(*s->units), sl_unit_place, place);
        free(place);
    } else if (s->count > 1) qsort(s->units, s->count, sizeof(*s->units), sl_unit_order);
    if (s->count > SIZE_MAX / sizeof(*s->inputs.units) || s->count > SIZE_MAX / sizeof(*s->inputs.scopes)) goto failed;
    s->inputs.units = calloc(s->count ? s->count : 1, sizeof(*s->inputs.units));
    s->inputs.scopes = calloc(s->count ? s->count : 1, sizeof(*s->inputs.scopes));
    if (!s->inputs.units || !s->inputs.scopes) goto failed;
    for (size_t i = 0; i < s->count; i++) s->units[i]->index = (uint32_t)i;
    if (!sl_freeze_sites(s)) { sl_env_fail(s, "cannot freeze bound reference sites", NULL); goto failed; }
    for (size_t i = 0; i < s->count; i++) {
        sl_unit *unit = s->units[i];
        if (unit->discovery->implicit_count) {
            if (unit->discovery->implicit_count > SIZE_MAX / sizeof(sh_package_link_edge)) goto failed;
            sh_package_link_edge *edges = calloc(unit->discovery->implicit_count, sizeof(*edges));
            if (!edges) goto failed;
            size_t plain = 0, conditional = unit->discovery->implicit_count;
            /* Implicit first, conditional last, in one allocation. */
            for (size_t j = 0; j < unit->discovery->implicit_count; j++) {
                sh_package_link_edge edge = {unit->discovery->implicit[j].slot, unit->discovery->implicit[j].target->index};
                if (unit->discovery->implicit[j].conditional) edges[--conditional] = edge;
                else edges[plain++] = edge;
            }
            unit->implicit_edges = edges;
            unit->unit->implicit = plain ? edges : NULL; unit->unit->implicit_count = plain;
            unit->unit->conditional = plain < unit->discovery->implicit_count ? edges + plain : NULL;
            unit->unit->conditional_count = unit->discovery->implicit_count - plain;
        }
        s->inputs.units[i] = *unit->unit; s->inputs.scopes[i] = unit->scope;
        unit->unit = s->inputs.units + i;
    }
#if defined(SH_PACKAGE_LIBRARY_TESTING) || defined(SH_PACKAGE_SOURCE_LINK_TESTING)
    if (!(s->frozen_rows = malloc((s->count ? s->count : 1) * sizeof(*s->frozen_rows)))) goto failed;
    memcpy(s->frozen_rows, s->inputs.units, s->count * sizeof(*s->frozen_rows));
#endif
    if (!sl_shared(s)) goto failed;
    SL_SHARED_LOG(s);
    if (s->reuse.out && !sl_reuse_build(s)) { s->environment = 1; goto failed; }
    sl_reuse_release(s);
    sl_discovery_release(s);
    sl_originals_release(s); /* the freeze: what the later phases held again goes too */
    sl_phase(s, 8, &mark);
    s->inputs.count = s->count; s->frozen = 1; s->error = NULL; s->error_capacity = 0;
    SL_CAPTURE_DUMP(s);
    sh_package_link_inputs_free(&roots); return s;
failed:
    if (!*error) snprintf(error, capacity, "scoped source dependency closure could not be completed");
    if (failure) sl_attribute(s, failure);
    sh_package_link_inputs_free(&roots); sh_package_source_link_close(s); return NULL;
}
sh_package_source_link *sh_package_source_link_open_roots(const sh_package_compilation *const *scopes, size_t count,
    const sh_resource_catalog *catalog, sh_package_baseline_reader original, void *original_context,
    sh_decl_registry_source registry, uintptr_t reflection, const sh_package_source_link_native *native,
    const sh_package_source_root_plan *root_plan, char *error, size_t capacity)
{
    return sh_package_source_link_open_attributed(scopes, count, catalog, original, original_context,
        registry, reflection, native, root_plan, NULL, error, capacity);
}
sh_package_source_link *sh_package_source_link_open(const sh_package_compilation *const *scopes, size_t count,
    const sh_resource_catalog *catalog, sh_package_baseline_reader original, void *original_context,
    sh_decl_registry_source registry, uintptr_t reflection, const sh_package_source_link_native *native,
    char *error, size_t capacity)
{
    return sh_package_source_link_open_roots(scopes, count, catalog, original, original_context,
        registry, reflection, native, NULL, error, capacity);
}
const sh_package_link_inputs *sh_package_source_link_inputs(const sh_package_source_link *source)
{ return source ? &source->inputs : NULL; }
/* Each frozen unit keeps its own record (sl_unit.unit), which the freeze copied into the inputs table and nothing
 * changes after it. */
const sh_package_link_unit *sh_package_source_link_unit(const sh_package_source_link *source, size_t index)
{ return source && source->frozen && index < source->count ? source->units[index]->unit : NULL; }
sh_package_link_unit *sh_package_source_link_lend_units(sh_package_source_link *source, size_t capacity)
{
    if (!source || !source->frozen || capacity < source->count) return NULL;
    sh_package_link_unit *rows = source->table ? source->table : source->inputs.units;
    size_t had = source->table ? source->table_capacity : source->count;
    if (!rows || capacity > SIZE_MAX / sizeof(*rows) || SL_REFUSED(g_sl_take_refusal)) return NULL;
    if (capacity <= had) { source->table = rows; source->table_capacity = had; source->inputs.units = NULL; return rows; }
    sh_package_link_unit *table = realloc(rows, capacity * sizeof(*table));
    if (!table) return NULL;
    memset(table + had, 0, (capacity - had) * sizeof(*table));
    /* The table may have moved: every frozen unit not preserved reads its row at its index again. */
    for (size_t i = 0; i < source->count; i++) if (!source->units[i]->preserved) source->units[i]->unit = table + i;
    source->table = table; source->table_capacity = capacity; source->inputs.units = NULL;
    return table;
}
int sh_package_source_link_preserve_unit(sh_package_source_link *source, size_t index)
{
    if (!source || !source->frozen) return 0;
    if (index >= source->count) return 1; /* an appended row: no captured record to keep */
    sl_unit *unit = source->units[index];
    if (unit->preserved) return 1;
    sh_package_link_unit *kept = SL_REFUSED(g_sl_preserve_refusal) ? NULL : malloc(sizeof(*kept));
    if (!kept) return 0;
#if defined(SH_PACKAGE_LIBRARY_TESTING) || defined(SH_PACKAGE_SOURCE_LINK_TESTING)
    g_sl_rows_checked++; g_sl_rows_kept++;
    if (!sl_row_same(unit->unit, source->frozen_rows + index)) g_sl_rows_changed++;
#endif
    *kept = *unit->unit; unit->unit = kept; unit->preserved = 1;
    return 1;
}
const char *sh_package_source_link_counters(const sh_package_source_link *s, char *out, size_t capacity)
{
    if (!out || !capacity) return out;
    *out = 0;
    if (!s) return out;
    int n = snprintf(out, capacity, "source link %zu units discovered, %zu replayed from another scope "
        "(%zu recorded, %zu declined); %zu file binds, %zu of unsupplied originals; %zu originals captured "
        "(%llu MB); reverse base %zu units%s; discovery: file selection %llu ms, reads %llu ms, "
        "constructor requests %llu ms; %zu replayed from earlier compiles (memo %s); %zu stock campaign-only "
        "references kept as identities", s->census.units,
        s->census.replayed, s->census.recorded,
        s->census.declined, s->census.binds, s->census.original_binds, s->census.originals,
        s->census.original_bytes >> 20, s->census.reverse_units,
        s->census.reverse_retained ? " (rebuilt from the retained reverse graph)" : "", s->census.select_us / 1000u,
        s->census.read_us / 1000u, s->census.construct_us / 1000u, s->census.retained,
        s->census.memo == 1 ? "kept" : s->census.memo == 2 ? "renewed" : s->census.memo == 3 ? "busy" : "unused",
        s->census.campaign_boundaries);
    /* The discovery sub-phases, on a line of their own. */
    if (n > 0 && (size_t)n < capacity)
        snprintf(out + n, capacity - (size_t)n, "\nsource link detail: material companions %llu ms within file "
            "selection; within reads, replay validation %llu ms over %zu recordings, replay application %llu ms, "
            "fresh reads %llu ms; originals captured in %llu ms and hashed in %llu ms",
            s->census.companion_us / 1000u, s->census.applies_us / 1000u, s->census.replay_tries,
            s->census.apply_us / 1000u, s->census.fresh_us / 1000u, s->census.original_us / 1000u,
            s->census.identify_us / 1000u);
    size_t used = strlen(out);
    if (used < capacity)
        snprintf(out + used, capacity - used, "\nsource link lookup memo: %zu lookups (%zu by marked paths), %zu "
            "searches, %zu hits; "
            "%zu identities, %zu computations, %zu body hashes, %zu hits; %zu entries, %zu bytes, %zu peak bytes, %zu limit; "
            "replay validation without first original capture %llu ms",
            s->lookup_counts.lookups, s->lookup_counts.marked, s->lookup_counts.searches, s->lookup_counts.lookup_hits,
            s->lookup_counts.identities, s->lookup_counts.identity_computations, s->lookup_counts.body_hashes,
            s->lookup_counts.identity_hits,
            s->lookup.count, s->lookup.bytes, s->lookup.peak, s->lookup.limit,
            s->census.applies_without_capture_us / 1000u);
    /* Shared stock and streamed originals (R-344 stage A). */
    used = strlen(out);
    if (used < capacity)
        snprintf(out + used, capacity - used, "\nsource link shared stock: mode %d; %zu folds decided %zu units "
            "(%zu joined a baseline unit, %zu became one), %zu shared stock units, %zu promoted, %zu given back in %zu "
            "privatizations; %zu kept their own (closure %zu, inputs %zu, family %zu, keys %zu, structure %zu, "
            "certificate %zu); %zu certificates (%zu with a folded stock unit's baseline answers) in %llu ms, "
            "%zu key scans in %llu ms, %zu conductors, folds %llu ms; "
            "live units peak %zu, now %zu; streamed originals %zu (%llu MB), read again %zu (%llu MB, %llu ms)",
            s->share.mode, s->share.folds, s->share.decided, s->share.folded, s->share.rehomed, s->share.stock_units,
            s->share.promoted, s->share.privatized, s->share.privatizations, s->share.survivors,
            s->share.blocked_closure, s->share.blocked_inputs, s->share.blocked_family, s->share.blocked_keys,
            s->share.blocked_equal, s->share.blocked_local, s->share.certified, s->share.certificates_reused,
            s->share.certify_us / 1000u,
            s->share.key_scans, s->share.keys_us / 1000u, s->share.conductors, s->share.fold_us / 1000u,
            s->share.peak_live, s->share.live, s->share.released, s->share.released_bytes >> 20, s->share.rereads,
            s->share.reread_bytes >> 20, s->share.reread_us / 1000u);
    /* Unchanged-scope reuse (R-348). */
    used = strlen(out);
    if (used < capacity && (s->reuse.counts.scopes_keyed || s->reuse.counts.scopes_offered)) {
        const sh_package_source_reuse_counters *r = &s->reuse.counts;
        snprintf(out + used, capacity - used, "\nsource link scope reuse: %zu scopes keyed in %llu ms, %zu offered, %zu "
            "matched, %zu restored; %zu bindings offered, %zu valid, %zu bound (%zu admission copies replaced, %zu stock "
            "units certified here), refused: own %zu, absent %zu, structure %zu, family %zu, changed %zu, inputs %zu, "
            "closure %zu, certificate %zu; offers judged in %llu ms; recorded %zu scopes, %zu bindings, %zu stock "
            "entries in %llu ms; memory: keying %+lld MB private, working state %zu KB at the freeze (marks %zu KB), "
            "offered record %zu KB, new record %zu KB", r->scopes_keyed, r->key_us / 1000u, r->scopes_offered,
            r->scopes_matched, r->scopes_restored, r->bindings_offered, r->bindings_valid, r->bindings_bound,
            r->copies_dropped, r->certified, r->refused[0], r->refused[1], r->refused[2], r->refused[3], r->refused[4],
            r->refused[5], r->refused[6], r->refused[7], r->validate_us / 1000u, r->scopes_recorded, r->bindings_recorded,
            r->stock_recorded, r->record_us / 1000u, r->key_private_mb, r->working_bytes >> 10, r->mark_bytes >> 10,
            r->offered_bytes >> 10, r->record_bytes >> 10);
    }
    /* Which stock-route references were kept as identities (sl_files): each
     * name once, with the scopes that kept it; the first 16 names. */
    for (size_t i = 0, listed = 0; i < s->count && listed < 16; i++) {
        const sl_unit *unit = s->units[i], *earlier = NULL;
        if (!unit || !unit->campaign_boundary || unit->unit->file_count || !unit->unit->original_binding) continue;
        for (size_t j = 0; j < i && !earlier; j++) {
            const sl_unit *other = s->units[j];
            if (other && other->campaign_boundary && !other->unit->file_count && other->unit->original_binding &&
                !strcmp(other->unit->family, unit->unit->family) && !strcmp(other->unit->name, unit->unit->name)) earlier = other;
        }
        if (earlier) continue;
        used = strlen(out);
        if (used + 1 >= capacity) break;
        snprintf(out + used, capacity - used, "%s%s %s [scopes", listed++ ? "; " : "\nstock campaign-only references kept: ",
            unit->unit->family, unit->unit->name);
        for (size_t j = i; j < s->count; j++) {
            const sl_unit *other = s->units[j];
            if (!other || !other->campaign_boundary || other->unit->file_count || !other->unit->original_binding ||
                strcmp(other->unit->family, unit->unit->family) || strcmp(other->unit->name, unit->unit->name)) continue;
            used = strlen(out);
            if (used + 1 >= capacity) break;
            snprintf(out + used, capacity - used, " %zu", (size_t)other->scope);
        }
        used = strlen(out);
        if (used + 1 < capacity) snprintf(out + used, capacity - used, "]");
    }
    return out;
}
int sh_package_source_link_unit_replayed(const sh_package_source_link *s, size_t index)
{ return s && s->frozen && index < s->count && s->units[index]->replayed; }
void sh_package_source_link_lookup_counters(const sh_package_source_link *s,
    sh_package_source_lookup_counters *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!s) return;
    *out = s->lookup_counts;
    out->entries = s->lookup.count; out->bytes = s->lookup.bytes;
    out->peak_bytes = s->lookup.peak; out->limit = s->lookup.limit;
    out->original_captures = s->census.originals;
    out->original_bytes = s->census.original_bytes; out->original_capture_us = s->census.original_us;
    out->replay_validation_us = s->census.applies_us;
    out->replay_validation_without_capture_us = s->census.applies_without_capture_us;
}
int sh_package_source_link_identity(sh_package_source_link *s, size_t index,
    const sh_compiled_resource *file, sh_package_file_identity *out, char *error, size_t capacity)
{
    sl_original *original = s && file ? sl_original_peek(s, file->engine_path) : NULL;
    if (original && &original->resource == file && (original->resource.body || sl_original_detached(original))) {
        if (!original->identified) {
            unsigned char *owned = NULL;
            const unsigned char *bytes = sl_original_bytes(s, original, &owned, error, capacity);
            int ok = bytes && sh_package_bytes_identity(bytes, original->resource.body_length, &original->identity);
            free(owned);
            if (!ok) {
                if (out) memset(out, 0, sizeof(*out));
                if (bytes && error && capacity) snprintf(error, capacity, "cannot identify captured original: %s", file->engine_path);
                return 0;
            }
            original->identified = 1;
        }
        *out = original->identity; return 1;
    }
    if (!s || index >= s->count) {
        if (out) memset(out, 0, sizeof(*out));
        if (error && capacity) snprintf(error, capacity, "resource identity has no source unit");
        return 0;
    }
    size_t scope = s->units[index]->scope;
    if (out && file && s->units[index]->unit->compilation == s->scopes[scope] && sl_supplied_file(s, scope, file))
        return sl_lookup_identity(s, scope, file, out, error, capacity);
    return sh_package_compilation_identity(s->units[index]->unit->compilation, file, out, error, capacity);
}
const char *sh_package_source_link_memory(const sh_package_source_link *source, char *out, size_t capacity)
{
    size_t used = 0;
    if (!out || !capacity) return out;
    *out = 0;
    for (size_t i = 0; source && i < source->memory_call_count && used < capacity; i++) {
        const unsigned long long *mb = source->memory_calls[i].mb;
        int n = snprintf(out + used, capacity - used, "%s%s %llu/%llu", used ? ", " : "",
            sl_phase_names[source->memory_calls[i].phase], mb[0], mb[1]);
        if (n < 0) break;
        used += (size_t)n;
    }
    return out;
}
const char *sh_package_source_link_phases(const sh_package_source_link *source, char *out, size_t capacity)
{
    size_t used = 0;
    if (!out || !capacity) return out;
    *out = 0;
    for (int i = 0; source && i < 9 && used < capacity; i++) {
        int n = snprintf(out + used, capacity - used, "%s%s %llu ms", i ? ", " : "", sl_phase_names[i], source->phase_ms[i]);
        if (n < 0) break;
        used += (size_t)n;
    }
    return out;
}
const unsigned char *sh_package_source_link_shared(const sh_package_source_link *source)
{ return source && source->frozen ? source->shared : NULL; }
int sh_package_source_link_presentation(const sh_package_source_link *source, size_t index)
{
    return source && source->frozen && index < source->count && source->units[index] &&
        source->units[index]->presentation;
}
const char *sh_package_source_link_original_note(const sh_package_source_link *source, size_t index)
{
    if (!source || index >= source->count || !source->units[index] || !SL_EXTRA(source->units[index])->original_note)
        return "";
    return source->units[index]->extra->original_note;
}
typedef struct sl_compile { sh_package_source_link *source; const sh_package_link_adapter *native; } sl_compile;
int sh_package_source_link_sites(const sh_package_source_link *s, size_t index,
    const sh_compiled_resource *file, const sh_package_link_edge **sites, size_t *count)
{
    size_t k = 0, begin, end;
    if (sites) *sites = NULL;
    if (count) *count = 0;
    if (!s || !s->frozen || index >= s->count || !file || !sites || !count) return 0;
    const sl_unit *unit = s->units[index];
    while (k < unit->discovered_count && unit->discovered[k] != file) k++;
    if (k == unit->discovered_count) return 0;
    /* A file's sites were recorded contiguously while it was bound: its first run. */
    sl_site_range(unit, file, &begin, &end);
    *sites = end > begin ? unit->site_edges + begin : NULL; *count = end - begin;
    return 1;
}
const char *sh_package_source_link_fsm_config(const sh_package_source_link *s, size_t unit)
{
    return s && s->frozen && unit < s->count ? SL_EXTRA(s->units[unit])->fsm_config : NULL;
}
int sh_package_source_link_fsm_stock(const sh_package_source_link *s, const char *name)
{
    return s && name && s->fsm_names_ready && sl_fsm_listed(s, name);
}
/* A scope's own frozen unit of this family and name: a member of the scope, never a shared stock unit that no
 * baseline unit reaches (stock_only). */
int sh_package_source_link_find_unit(sh_package_source_link *s, size_t scope,
    const char *family, const char *name, size_t *unit)
{
    sl_unit *found = s && s->frozen && scope < s->scope_count && family && *family && name && *name ?
        sl_find_raw(s, scope, family, name) : NULL;
    if (found && found->stock_only) found = NULL;
    if (unit) *unit = found ? found->index : SIZE_MAX;
    return found != NULL;
}
/* What unit's scope binds for this family and name: its own unit, else (shared stock) the baseline unit it binds
 * instead; a baseline member binds members only, a stock_only unit any baseline unit. */
static sl_unit *sl_frozen_resolve(const sh_package_source_link *s, const sl_unit *from, const char *family, const char *name)
{
    sl_unit *found = sl_find_raw(s, from->scope, family, name);
    if (from->scope == s->share.baseline) return found && found->stock_only && !from->stock_only ? NULL : found;
    return found ? found : sl_share_find(s, from->scope, family, name);
}
int sh_package_source_link_stock_only(const sh_package_source_link *s, size_t index)
{ return s && s->frozen && index < s->count && s->units[index]->stock_only; }
int sh_package_source_link_shared_stock(const sh_package_source_link *s, size_t index)
{ return s && s->frozen && index < s->count && s->units[index]->shared_stock; }
size_t sh_package_source_link_scope_shared(const sh_package_source_link *s, size_t scope)
{ return s && s->frozen && s->share.maps && scope < s->scope_count ? s->share.maps[scope].count : 0; }
size_t sh_package_source_link_scope_shared_unit(const sh_package_source_link *s, size_t scope, size_t i)
{
    return i < sh_package_source_link_scope_shared(s, scope) ? s->share.maps[scope].units[i]->index : SIZE_MAX;
}
int sh_package_source_link_stock_facts(sh_package_source_link *s, size_t index, int *keys, int *conductor)
{
    const sl_stock_fact *fact;
    *keys = *conductor = -1;
    if (!s || !s->frozen || index >= s->count || !(fact = sl_stock_fact_of(s, s->units[index]))) return 0;
    *keys = fact->keys; *conductor = fact->conductor;
    return 1;
}
int sh_package_source_link_scope_find(const sh_package_source_link *s, size_t scope, const char *family,
    const char *name, size_t *unit)
{
    const sl_unit *found = s && s->frozen && scope < s->scope_count && scope != s->share.baseline && family && name ?
        sl_share_find(s, scope, family, name) : NULL;
    if (unit) *unit = found ? found->index : SIZE_MAX;
    return found != NULL;
}
#if defined(SH_PACKAGE_LIBRARY_TESTING) || defined(SH_PACKAGE_SOURCE_LINK_TESTING) || \
    defined(SH_RESOURCE_GRAPH_TESTING) || defined(SH_PACKAGE_RUNTIME_TESTING)
size_t sh_package_source_link_test_share_rep(const sh_package_source_link *s, size_t index)
{
    if (!s || !s->frozen || index >= s->count || s->share.mode != 2) return SIZE_MAX;
    const sl_unit *rep = sl_rep_of(s, s->units[index]);
    return rep ? rep->index : SIZE_MAX;
}
#endif
int sh_package_source_link_resolve_unit(sh_package_source_link *s, size_t unit,
    const char *family, const char *name, size_t *target, char *error, size_t capacity)
{
    char fallback[1024]; sl_unit *found = NULL;
    if (!error || !capacity) { error = fallback; capacity = sizeof(fallback); }
    *error = 0;
    if (target) *target = SIZE_MAX;
    if (s && s->frozen && target && unit < s->count && family && *family && name && *name)
        found = sl_frozen_resolve(s, s->units[unit], family, name);
    if (!found) {
        snprintf(error, capacity, "binding is outside the captured source scope: %s/%s",
            family ? family : "", name ? name : ""); return 0;
    }
    *target = found->index; return 1;
}

unsigned char *sh_package_source_link_bind_derived(sh_package_source_link *s, size_t index,
    const unsigned char *body, size_t length, sh_package_binding_resolver reference,
    void *reference_context, sh_package_bound_exists exists, void *exists_context,
    size_t *output_length, char *error, size_t capacity)
{
    char fallback[1024]; unsigned char *out;
    if (!error || !capacity) { error = fallback; capacity = sizeof(fallback); }
    *error = 0;
    if (output_length) *output_length = 0;
    if (!s || !s->frozen || index >= s->count || !body || !reference || !output_length) {
        snprintf(error, capacity, "binding requires a captured source unit"); return NULL;
    }
    sl_unit *unit = s->units[index];
    sh_package_source_binding result = {0};
    char *old_error = s->error; size_t old_capacity = s->error_capacity;
    s->error = error; s->error_capacity = capacity; s->failed = 0;
    out = sh_package_source_bind_derived(s->readers[unit->scope], unit->unit->family, unit->unit->name,
        body, length, reference, reference_context, exists, exists_context, output_length, &result, error, capacity);
    if (s->failed) { free(out); out = NULL; *output_length = 0; }
    unit->derived_coverage = out && result.coverage;
    s->error = old_error; s->error_capacity = old_capacity; return out;
}

unsigned char *sh_package_source_link_bind_unit(sh_package_source_link *s, size_t index,
    const unsigned char *body, size_t length, sh_package_binding_resolver reference,
    void *reference_context, size_t *output_length, char *error, size_t capacity)
{
    return sh_package_source_link_bind_derived(s, index, body, length, reference,
        reference_context, NULL, NULL, output_length, error, capacity);
}

int sh_package_source_link_key_space(sh_package_source_link *s, sh_package_key_space *space)
{
    if (!s || !s->scope_count || !s->readers || !s->readers[0]) return -1;
    return sh_package_source_key_space(s->readers[0], space);
}

int sh_package_source_link_bind_keys(sh_package_source_link *s, size_t index,
    const unsigned char *body, size_t length, sh_package_key_site_resolver resolver, void *context,
    unsigned char **output, size_t *output_length, char *error, size_t capacity)
{
    char fallback[1024]; int status;
    if (!error || !capacity) { error = fallback; capacity = sizeof(fallback); }
    *error = 0;
    if (output) *output = NULL;
    if (output_length) *output_length = 0;
    if (!s || !s->frozen || index >= s->count || !body || !resolver || !output || !output_length) {
        snprintf(error, capacity, "registration keys require a captured source unit"); return -1;
    }
    sl_unit *unit = s->units[index];
    char *old_error = s->error; size_t old_capacity = s->error_capacity;
    s->error = error; s->error_capacity = capacity; s->failed = 0;
    status = sh_package_source_bind_keys(s->readers[unit->scope], unit->unit->family, unit->unit->name,
        body, length, resolver, context, output, output_length, NULL, error, capacity);
    if (s->failed && status == 1) { free(*output); *output = NULL; *output_length = 0; status = -1; }
    s->error = old_error; s->error_capacity = old_capacity; return status;
}

unsigned char *sh_package_source_link_bind_strings(sh_package_source_link *s, size_t index,
    const unsigned char *body, size_t length, sh_package_string_site_resolver reference,
    void *context, size_t *output_length, char *error, size_t capacity)
{
    char fallback[1024]; unsigned char *out;
    if (!error || !capacity) { error = fallback; capacity = sizeof(fallback); }
    *error = 0;
    if (output_length) *output_length = 0;
    if (!s || !s->frozen || index >= s->count || !body || !reference || !output_length) {
        snprintf(error, capacity, "localization requires a captured source unit"); return NULL;
    }
    sl_unit *unit = s->units[index];
    char *old_error = s->error; size_t old_capacity = s->error_capacity;
    s->error = error; s->error_capacity = capacity; s->failed = 0;
    out = sh_package_source_bind_strings(s->readers[unit->scope], unit->unit->family, unit->unit->name,
        body, length, reference, context, output_length, NULL, error, capacity);
    if (s->failed) { free(out); out = NULL; *output_length = 0; }
    s->error = old_error; s->error_capacity = old_capacity; return out;
}

int sh_package_source_link_coverage(const sh_package_source_link *s, size_t index, int derived)
{
    if (!s || index >= s->count) return 0;
    return derived ? s->units[index]->derived_coverage : s->units[index]->coverage;
}

unsigned char *sh_package_source_link_bind_file(sh_package_source_link *s, size_t index,
    const sh_compiled_resource *file, const unsigned char *body, size_t length,
    sh_package_binding_resolver reference, void *context, size_t *output_length,
    char *error, size_t capacity)
{
    char fallback[1024]; unsigned char *out = NULL;
    if (!error || !capacity) { error = fallback; capacity = sizeof(fallback); }
    *error = 0; if (output_length) *output_length = 0;
    if (!s || !s->frozen || index >= s->count || !file || !body || !reference || !output_length) {
        snprintf(error, capacity, "file binding requires a captured source unit"); return NULL;
    }
    sl_unit *unit = s->units[index]; int member = 0;
    for (size_t i = 0; i < unit->unit->file_count; i++) if (unit->unit->files[i] == file) member = 1;
    if (!member) { snprintf(error, capacity, "file is outside its captured source unit"); return NULL; }
    char *old_error = s->error; size_t old_capacity = s->error_capacity;
    s->error = error; s->error_capacity = capacity; s->failed = 0;
    out = sl_bind_member(s, unit, file, body, length, reference, context, output_length, error, capacity);
    if (s->failed) { free(out); out = NULL; *output_length = 0; }
    s->error = old_error; s->error_capacity = old_capacity; return out;
}
typedef struct sl_shader_linked_paths {
    sl_unit *unit;
    char **names;
    sh_package_shader_path_visitor visitor;
    void *context;
} sl_shader_linked_paths;
static int sl_shader_linked_path(void *context, sh_package_shader_path_kind kind,
    const char *original, const char *linked)
{
    sl_shader_linked_paths *walk = context;
    if (kind == SH_PACKAGE_SHADER_PERMUTATION) {
        size_t p;
        for (p = 0; p < SL_EXTRA(walk->unit)->program_count; p++) if (!strcmp(walk->unit->extra->programs[p].name, original)) break;
        if (p == SL_EXTRA(walk->unit)->program_count) return 0;
        if (walk->names[p] && strcmp(walk->names[p], linked)) return 0;
        if (!walk->names[p] && !(walk->names[p] = _strdup(linked))) return 0;
    }
    return walk->visitor(walk->context, kind, original, linked);
}
int sh_package_source_link_shader_paths(sh_package_source_link *s, size_t index,
    const char *name, sh_package_shader_path_visitor visitor, void *context, char *error, size_t capacity)
{
    char fallback[1024]; int ok = 0;
    if (!error || !capacity) { error = fallback; capacity = sizeof(fallback); }
    *error = 0;
    if (!s || !s->frozen || index >= s->count || !name || !visitor || !s->native.shader ||
        strcmp(s->units[index]->unit->family, "renderprog") || !SL_EXTRA(s->units[index])->program_count) {
        snprintf(error, capacity, "shader paths require a captured file-backed program unit"); return 0;
    }
    sl_unit *unit = s->units[index];
    const sl_extra *extra = unit->extra; /* a renderprog unit with programs has one */
    char **names = calloc(extra->program_count, sizeof(*names));
    if (!names) goto done;
    names[0] = _strdup(name);
    sl_shader_linked_paths walk = {unit, names, visitor, context};
    for (size_t p = 0; p < extra->program_count; p++) {
        if (!names[p] || sh_package_shader_paths(s->native.shader->renderer, extra->programs[p].name,
            names[p], &extra->programs[p].decoded, sl_shader_linked_path, &walk, error, capacity) != SH_PACKAGE_SHADER_OK) goto done;
    }
    ok = 1;
done:
    if (names) for (size_t p = 0; p < extra->program_count; p++) free(names[p]);
    free(names);
    if (!ok && !*error) snprintf(error, capacity, "shader paths exceed captured identity or allocation bounds");
    return ok;
}

static int sl_resolve(void *context, size_t unit, const char *family, const char *name,
    size_t *target, char *error, size_t capacity)
{
    sl_compile *compile = context;
    return sh_package_source_link_resolve_unit(compile->source, unit, family, name, target, error, capacity);
}
static unsigned char *sl_bind(void *context, size_t index, const sh_compiled_resource *file,
    const unsigned char *body, size_t length, sh_package_link_reference reference, void *reference_context,
    size_t *output_length, char *error, size_t capacity)
{
    sl_compile *compile = context;
    return sh_package_source_link_bind_file(compile->source, index, file, body, length,
        reference, reference_context, output_length, error, capacity);
}

int sh_package_source_link_identify(sh_package_source_link *s,
    sh_package_link_classes *out, char *error, size_t capacity)
{
    char fallback[1024];
    if (!error || !capacity) { error = fallback; capacity = sizeof(fallback); }
    *error = 0;
    if (!s || !s->frozen || (!s->inputs.units && s->inputs.count)) {
        sh_package_link_classes_free(out);
        snprintf(error, capacity, s && s->frozen ? "the captured units were handed to a library build" :
            "resource identity requires a captured source scope"); return 0;
    }
    sl_compile compile = {s, NULL};
    sh_package_link_adapter adapter = {&compile, sl_resolve, sl_bind, NULL, NULL};
    return sh_package_link_identify_units(s->inputs.units, s->inputs.count, &adapter, out, error, capacity);
}
static int sl_assign(void *context, size_t unit, const unsigned char identity[32], int original_binding,
    const sh_compiled_resource *const *files, size_t count, char **name, char ***paths, char *error, size_t capacity)
{
    sl_compile *compile = context;
    return compile->native->assign(compile->native->context, unit, identity, original_binding,
        files, count, name, paths, error, capacity);
}
static int sl_assign_members(void *context, size_t unit, const char *name,
    const sh_compiled_resource *const *files, const char *const *paths, size_t count,
    char ***names, char *error, size_t capacity)
{
    sl_compile *compile = context;
    return compile->native->assign_members(compile->native->context, unit, name,
        files, paths, count, names, error, capacity);
}
int sh_package_source_link_compile(sh_package_source_link *s, const sh_package_sources *inventory,
    const sh_package_link_adapter *native, const sh_package_policy *policy, char *(*canonical_path)(const char *),
    sh_package_link_output *out, char *error, size_t capacity)
{
    char fallback[1024];
    if (!error || !capacity) { error = fallback; capacity = sizeof(fallback); }
    *error = 0;
    if (!s || !s->frozen || !native || !native->assign) {
        sh_package_link_output_free(out); snprintf(error, capacity, "source linker has no admitted native assignment"); return 0;
    }
    if (!s->inputs.units && s->inputs.count) {
        sh_package_link_output_free(out); snprintf(error, capacity, "the captured units were handed to a library build"); return 0;
    }
    sl_compile compile = {s, native};
    sh_package_link_adapter adapter = {&compile, sl_resolve, sl_bind, sl_assign, native->registry};
    if (native->assign_members) adapter.assign_members = sl_assign_members;
    s->error = error; s->error_capacity = capacity; s->failed = 0;
    int ok = sh_package_link(inventory, s->inputs.units, s->inputs.count, &adapter, policy,
        canonical_path, out, error, capacity);
    s->error = NULL; s->error_capacity = 0; return ok;
}
