/* Dependencies observed at native resource parsing and provider boundaries. */
#ifndef SH_RESOURCE_GRAPH_H
#define SH_RESOURCE_GRAPH_H
#include <stddef.h>
#include <stdint.h>

typedef struct sh_resource_graph_frame {
    uint64_t context, serial;
    size_t node;
    size_t generation;
    unsigned phase;
} sh_resource_graph_frame;
/* Frames are value tokens only. Owned scope state follows the execution
 * fiber; no graph record points into a caller's stack. */

/* Native hooks bracket each actual parse. A new source parse invalidates the
 * previous source and deferred-state edges. Entity definitions require both
 * phases before a walk can claim all recorded phases are available. */
void sh_resource_graph_begin(sh_resource_graph_frame *frame, const char *type, const char *name);
/* Canonical entity state has a separate edge set; it never replaces source
 * edges. Results started before a source reload cannot complete a newer node. */
void sh_resource_graph_begin_state(sh_resource_graph_frame *frame, const char *type, const char *name,
                                    int expanded_inheritance);
/* Begin a static prepared-state fallback only when no complete or active
 * state record exists and its source is idle. Returns 1 with an active frame;
 * return 0 leaves the current scope unchanged and must not be paired with end.
 * It can add partial known edges without replacing a complete native capture. */
int sh_resource_graph_begin_missing_state(sh_resource_graph_frame *frame,
    const char *type, const char *name, int expanded_inheritance);
/* Ignore instance-specific references inside this scope. Nested native source
 * parses still record their own dependencies without attaching to the caller. */
void sh_resource_graph_pause(sh_resource_graph_frame *frame);
void sh_resource_graph_end(sh_resource_graph_frame *frame, int parsed);
/* End a scope without publishing its collected edges. A prepared-state read
 * that fails before parsing uses this to preserve earlier partial evidence. */
void sh_resource_graph_discard(sh_resource_graph_frame *frame);
void sh_resource_graph_reference(const char *type, const char *name);
/* A native read whose identity cannot be observed invalidates only the active
 * phase; its other observed dependencies remain available. */
void sh_resource_graph_incomplete(void);
/* Positive nesting check before a scoped observer publishes a receipt. */
int sh_resource_graph_frame_current(const sh_resource_graph_frame *frame);
void sh_resource_graph_expanded_inheritance(sh_resource_graph_frame *frame, int expanded);
/* A source frame that also records what a hooked lookup asked for and did not find (a
 * retirement default's frame, R-321), so a later addition of that identity reaches this node
 * as a recorded consumer. Nested frames do not inherit it; elsewhere a lookup that finds
 * nothing records nothing. */
void sh_resource_graph_begin_requests(sh_resource_graph_frame *frame, const char *type, const char *name);
/* The same for canonical deferred entity state; missing names belong to that
 * phase without replacing its source dependencies. */
void sh_resource_graph_begin_state_requests(sh_resource_graph_frame *frame, const char *type,
    const char *name, int expanded_inheritance);
int sh_resource_graph_recording_requests(void);
void sh_resource_graph_request(const char *type, const char *name);
void sh_resource_graph_file(const char *path);
/* Replace one product stage's exact file inputs without overwriting native
 * parse or other producer edges. No package metadata is involved. Empty input
 * removes this stage. Source reload clears all stages until republished.
 * Failure preserves earlier edges and makes graph walks report ERROR. */
int sh_resource_graph_producer_inputs(const char *producer, const char *path,
    const char *const *inputs, size_t count);
int sh_resource_graph_recording(void);
/* A lower-level source parser can share the surrounding generic load scope.
 * Only the current scope counts; a paused or deferred-state scope never does. */
int sh_resource_graph_source_active(const char *type, const char *name);

/* Return positive to follow an edge, zero to prune it, negative to abort.
 * Empty type denotes an exact engine provider path. The root has NULL parent.
 * An expanded entity state already consumes its inherited fields: parent
 * definition edges then require source parsing, not separate parent instances.
 * Missing phases make the result false but do not stop visiting known edges.
 * Visitor runs under the graph read lock: never call the engine or graph APIs. */
typedef int (*sh_resource_graph_visitor)(void *context, const char *parent_type,
    const char *parent_name, const char *type, const char *name);
int sh_resource_graph_walk(const char *type, const char *name,
    sh_resource_graph_visitor visitor, void *context);
/* Separate missing coverage from an interrupted walk. Missing roots/phases
 * return INCOMPLETE after visiting every available edge. Allocation failure,
 * failed graph recording or visitor abort returns ERROR. */
typedef enum sh_resource_graph_status {
    SH_RESOURCE_GRAPH_ERROR = -1,
    SH_RESOURCE_GRAPH_INCOMPLETE = 0,
    SH_RESOURCE_GRAPH_COMPLETE = 1
} sh_resource_graph_status;
sh_resource_graph_status sh_resource_graph_walk_status(const char *type, const char *name,
    sh_resource_graph_visitor visitor, void *context);
typedef struct sh_resource_graph_identity {
    char *type, *name; /* Empty type identifies a provider file. */
} sh_resource_graph_identity;
typedef struct sh_resource_graph_impact {
    sh_resource_graph_identity *items;
    size_t count;
} sh_resource_graph_impact;
/* Snapshot changed provider files and all recorded transitive consumers,
 * including source, entity state and product-stage edges. Names are copied,
 * sorted by type/name and valid after releasing the graph lock. This is a set,
 * not a reload order; cycles require the caller's activation protocol.
 *
 * COMPLETE covers only this recorded graph, never the whole live registry.
 * An unknown input or any incomplete/active recorded node returns INCOMPLETE
 * with known consumers retained: a missing edge may hide another consumer.
 * ERROR returns an empty result. No graph or native mutation occurs. Start
 * out empty; free an earlier result before reuse. No author settings apply. */
sh_resource_graph_status sh_resource_graph_affected(const char *const *paths,
    size_t count, sh_resource_graph_impact *out);
/* Same owned reverse closure with native identity seeds. An empty type is a
 * provider path. This allows catalog/native path mappings to seed consumers
 * even when the original native read preceded graph instrumentation. */
sh_resource_graph_status sh_resource_graph_consumers(
    const sh_resource_graph_identity *seeds, size_t count, sh_resource_graph_impact *out);
/* The same closure stopped at consumers the barrier admits: such a node is
 * neither returned nor followed unless it is itself a seed. Coverage is still
 * reported for the whole recorded graph. The barrier runs under the graph lock
 * and must not call back into the graph. */
typedef int (*sh_resource_graph_barrier)(void *context, const char *type, const char *name);
sh_resource_graph_status sh_resource_graph_consumers_bounded(
    const sh_resource_graph_identity *seeds, size_t count,
    sh_resource_graph_barrier barrier, void *context, sh_resource_graph_impact *out);
void sh_resource_graph_impact_free(sh_resource_graph_impact *impact);
/* The closure sh_resource_graph_consumers_bounded returns (seeds included), with coverage judged on
 * that closure only: COMPLETE when every seed has a node and every node the walk visits, including
 * barrier holders it stops at, has every recorded phase complete; else INCOMPLETE with the first
 * unobserved seed or incompletely observed consumer named in first. Never a whole-graph claim and
 * never a claim about consumers no recorded parse names. */
sh_resource_graph_status sh_resource_graph_consumers_observed(const sh_resource_graph_identity *seeds,
    size_t count, sh_resource_graph_barrier barrier, void *context, sh_resource_graph_impact *out,
    char *first, size_t capacity);
void sh_resource_graph_counts(size_t *nodes, size_t *complete, size_t *edges);
/* Read history kept against an identity across reparses (R-309, copied state). A reparse replaces a
 * node's edges and phases, never these bits. ESCAPED: a native size getter read the image's size, so a
 * native owner may hold a copy that an in-place rebuild does not refresh. PRECAPTURE: the image already
 * existed when size-read recording began, so reads before that are unknown. Marking interns the node. */
enum {
    SH_RESOURCE_GRAPH_HISTORY_ESCAPED = 1,
    SH_RESOURCE_GRAPH_HISTORY_PRECAPTURE = 2
};
/* A mark that cannot be recorded (allocation failure) invalidates the size history. */
void sh_resource_graph_history_mark(const char *type, const char *name, unsigned bits);
unsigned sh_resource_graph_history(const char *type, const char *name);
/* Begun once the size getters are hooked and every image then registered is marked PRECAPTURE; until
 * then no image can be shown free of copies. Invalidation is permanent for the process: a size read
 * whose image cannot be identified, or a lost mark, means no absence of reads can be shown any more,
 * and a later begin does not restore it. */
void sh_resource_graph_size_history_begin(void);
void sh_resource_graph_size_history_invalidate(void);
int sh_resource_graph_size_history_ready(void);

#ifdef SH_RESOURCE_GRAPH_TESTING
void sh_resource_graph_test_reset(void);
#endif
#endif
