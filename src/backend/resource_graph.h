/* Dependencies observed at native resource parsing and provider boundaries. */
#ifndef SH_RESOURCE_GRAPH_H
#define SH_RESOURCE_GRAPH_H
#include <stddef.h>
#include <stdint.h>

typedef struct sh_resource_graph_frame {
    size_t node;
    size_t *children;
    size_t count, capacity;
    size_t generation;
    unsigned phase;
    int expanded_inheritance;
    int failed;
    struct sh_resource_graph_frame *parent;
} sh_resource_graph_frame;

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
void sh_resource_graph_impact_free(sh_resource_graph_impact *impact);
void sh_resource_graph_counts(size_t *nodes, size_t *complete, size_t *edges);

#ifdef SH_RESOURCE_GRAPH_TESTING
void sh_resource_graph_test_reset(void);
#endif
#endif
