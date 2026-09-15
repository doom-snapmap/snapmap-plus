/* Ordered records owned by the native idDeclTypeInfoGraph reader. */
#ifndef SH_DECL_GRAPH_H
#define SH_DECL_GRAPH_H

#include <stdint.h>
#include "decl_tree.h"

typedef struct sh_decl_graph_text { const char *text; size_t length; } sh_decl_graph_text;
typedef enum sh_decl_graph_kind {
    SH_DECL_GRAPH_SUBGRAPH,
    SH_DECL_GRAPH_NODE,
    SH_DECL_GRAPH_LINK
} sh_decl_graph_kind;

typedef struct sh_decl_graph_record {
    sh_decl_graph_kind kind;
    const sh_decl_node *syntax, *state;
    sh_decl_graph_text class_name, name;
    /* Indices in the owning graph's records, not native serialized indices.
     * SIZE_MAX means not applicable. Endpoint lookup is exact, case sensitive,
     * and chooses the first node across subgraphs in authored order. Names
     * are not unique identities: every repeated record remains present. */
    size_t parent, start, end;
    uint32_t layers;
} sh_decl_graph_record;

typedef struct sh_decl_graph sh_decl_graph;

/* Parse the verified graph envelope and resolve its topology. Unknown envelope
 * fields, ambiguous layer names, unresolved endpoints and unrepresentable layer
 * masks refuse inspection. Object state stays opaque: its reflected class and
 * any custom readers still require separate interpretation. This is not a
 * complete native validity check and does not itself establish merge identities.
 * No engine calls, mutation, recursion limit or application payload quota. */
sh_decl_graph *sh_decl_graph_open(sh_decl_source source, char *error, size_t error_capacity);
const sh_decl_node *sh_decl_graph_syntax(const sh_decl_graph *graph);
const sh_decl_node *sh_decl_graph_state(const sh_decl_graph *graph);
size_t sh_decl_graph_count(const sh_decl_graph *graph);
const sh_decl_graph_record *sh_decl_graph_at(const sh_decl_graph *graph, size_t index);
void sh_decl_graph_close(sh_decl_graph *graph);

#endif
