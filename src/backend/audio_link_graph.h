/* Immutable typed audio bindings inside one or more authored source scopes. */
#ifndef SH_AUDIO_LINK_GRAPH_H
#define SH_AUDIO_LINK_GRAPH_H
#include "audio_bank_codec.h"
#include "package_link_graph.h"

typedef struct sh_audio_graph_bank {
    const char *path;
    sh_audio_bank_source source;
    const sh_audio_bank_codec *codec;
    int original; /* Exact captured original bytes, not mere availability. */
} sh_audio_graph_bank;
typedef struct sh_audio_graph_media {
    const char *path;
    sh_audio_bank_source source;
    int original;
} sh_audio_graph_media;
typedef struct sh_audio_graph_scope {
    /* Complete selected bank inventory in the same registration order the
     * native activation will use. Peers never supply missing definitions.
     * Duplicate objects retain their native ordered construction recipe; the
     * caller must publish and retain that complete recipe together. */
    const sh_audio_graph_bank *banks;
    size_t bank_count;
    void *context;
    /* Captured current-locale lookup, including packed originals. 1 found,
     * 0 absent, -1 IO. Returned metadata/source stay valid through graph close. */
    int (*media)(void *context, uint32_t id, sh_audio_graph_media *out,
        char *error, size_t capacity);
} sh_audio_graph_scope;
typedef struct sh_audio_link_graph sh_audio_link_graph;

/* Build exact HIRC/media dependency identities using the common graph
 * partitioner. Original bank bytes remain borrowed and immutable. Returns
 * no partial graph, and never calls native loaders or writes authored files. */
sh_audio_link_graph *sh_audio_link_graph_open(const sh_audio_graph_scope *scopes,
    size_t count, char *error, size_t capacity);
void sh_audio_link_graph_close(sh_audio_link_graph *graph);
typedef struct sh_audio_graph_root {
    size_t node;
    unsigned char identity[32];
    int original;
} sh_audio_graph_root;
int sh_audio_link_graph_event(const sh_audio_link_graph *graph, size_t scope,
    uint32_t event, sh_audio_graph_root *out, char *error, size_t capacity);
/* Exact equality check over retained source bytes and resolved dependency
 * classes, suitable for certifying a positive outer sound fingerprint match. */
int sh_audio_link_graph_equal(const sh_audio_link_graph *graph, size_t left, size_t right);
/* Shared bus assignments use the same exact dependency partition as events.
 * Identical cooked IDs do not certify equal effect/state/modulator bindings.
 * record indexes the checked bank codec's bus_records array. Roots and values
 * stay valid through graph close; no numeric allocation is needed to compare.
 * No shared setting is selected or installed by this accessor. */
int sh_audio_link_graph_bus_record(const sh_audio_link_graph *graph, size_t scope,
    size_t bank, size_t record, sh_audio_graph_root *out);
/* Complete bus recipe identity, including all private dependencies. */
int sh_audio_link_graph_bus_object(const sh_audio_link_graph *graph,size_t scope,
    size_t bank,size_t object,sh_audio_graph_root *out);
/* Owned value bytes for a checked bus record, masked to that semantic field
 * when it shares a packed span. Exact identity and verification use the same
 * view. bound=1 requires a successful bind and relocates only typed private
 * references. Original bank bytes and complete object identities stay intact. */
int sh_audio_link_graph_bus_value(const sh_audio_link_graph *graph, size_t scope,
    size_t bank, size_t record, int bound, unsigned char **body, size_t *length,
    char *error, size_t capacity);
/* Traverse a retained graph root, including its exact source record and all
 * private dependencies. This also supports scalar bus roots with no edges. */
int sh_audio_link_graph_root_sources(const sh_audio_link_graph *graph, size_t node,
    int (*visit)(void *, const char *path, uint64_t offset, uint64_t length), void *context,
    char *error, size_t capacity);
/* Certify equivalence across retained providers once per graph pair. Reuses
 * the common exact partitioner over their already-collapsed classes, including
 * cycles and ordered definitions. Both immutable graphs/sources outlive this
 * comparison. Fingerprint agreement alone never authorizes a native ID reuse. */
typedef struct sh_audio_graph_comparison sh_audio_graph_comparison;
sh_audio_graph_comparison *sh_audio_link_graph_compare(const sh_audio_link_graph *left,
    const sh_audio_link_graph *right, char *error, size_t capacity);
int sh_audio_link_graph_compared(const sh_audio_graph_comparison *comparison, size_t left, size_t right);
void sh_audio_link_graph_comparison_close(sh_audio_graph_comparison *comparison);
/* Visit actual retained source records reachable from one scoped event. This
 * follows the resolved graph, including ordered definitions and native bank
 * dependencies, never all files in a peer scope. Records can share a path;
 * callers aggregate ownership without treating an entire container file as an
 * additional equality witness. The visitor's spans are borrowed and bounded. */
int sh_audio_link_graph_sources(const sh_audio_link_graph *graph, size_t scope, uint32_t event,
    int (*visit)(void *, const char *path, uint64_t offset, uint64_t length), void *context,
    char *error, size_t capacity);
int sh_audio_link_graph_bank_sources(const sh_audio_link_graph *graph, size_t scope, uint32_t bank,
    int (*visit)(void *, const char *path, uint64_t offset, uint64_t length), void *context,
    char *error, size_t capacity);
/* Every private definition and scoped optional absence needs a reserved ID.
 * The caller verifies native registry occupancy against baseline and retained
 * live ownership. absent means this scope has no definition, not a claim that
 * the native registry is empty. original+absent retains a verified original
 * reference's native lookup; private absence reserves an unoccupied name.
 * Neither creates a native object.
 * The callback receives the immutable graph/node as an exact witness, and must
 * reuse an exact private binding and refuse an occupied different private
 * binding. Certified originals retain their existing native name across scopes;
 * different capture closures do not create private engine objects. The graph
 * and its captured sources outlive any retained reservation. Package metadata
 * never supplies numeric registry IDs. */
typedef int (*sh_audio_graph_assign)(void *context, const sh_audio_link_graph *graph, size_t node,
    sh_audio_identity_domain domain,
    uint32_t authored, const unsigned char binding[32], int original, int absent,
    uint32_t *id, char *error, size_t capacity);
int sh_audio_link_graph_bind(sh_audio_link_graph *graph, sh_audio_graph_assign assign,
    void *context, char *error, size_t capacity);
/* Split binding lets dependent outputs finish their fallible preparation before
 * any graph IDs or bank streams change. The graph/sources must outlive the
 * transaction; callers serialize prepare/commit/cancel on a graph. Commit and
 * cancel both consume the transaction. Reservations remain caller-owned. */
typedef struct sh_audio_link_binding sh_audio_link_binding;
sh_audio_link_binding *sh_audio_link_graph_prepare_bind(sh_audio_link_graph *graph,
    sh_audio_graph_assign assign,void *context,char *error,size_t capacity);
int sh_audio_link_binding_bus_value(const sh_audio_link_binding *binding,
    size_t scope,size_t bank,size_t record,unsigned char **body,size_t *length,
    char *error,size_t capacity);
void sh_audio_link_graph_commit_bind(sh_audio_link_binding *binding);
void sh_audio_link_graph_cancel_bind(sh_audio_link_binding *binding);
/* bind replaces prepared outputs only after every reservation and bank edit
 * succeeds. Failure preserves the prior outputs. The caller rolls back its
 * reservation transaction; this module never changes native state. */
/* Borrowed immutable output for each scoped bank, present only after bind.
 * The caller owns transactional cache materialization and native activation. */
const sh_audio_bank_binding *sh_audio_link_graph_bank(const sh_audio_link_graph *graph,
    size_t scope, size_t bank);
/* The same transaction also prepares the private activation stream, with shared
 * controls/bus definitions moved out. An untouched original retains its native
 * stream. The full binding above remains available for shared bus recipes. */
const sh_audio_bank_binding *sh_audio_link_graph_private_bank(const sh_audio_link_graph *graph,
    size_t scope,size_t bank);
int sh_audio_link_graph_event_id(const sh_audio_link_graph *graph, size_t scope,
    uint32_t authored, uint32_t *id);
int sh_audio_link_graph_media_id(const sh_audio_link_graph *graph, size_t scope,
    uint32_t authored, uint32_t *id);
int sh_audio_link_graph_bank_id(const sh_audio_link_graph *graph, size_t scope,
    uint32_t authored, uint32_t *id);
#endif
