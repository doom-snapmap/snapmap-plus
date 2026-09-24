/* Equivalence of fully resolved resource bindings across authored packages. */
#ifndef SH_PACKAGE_LINK_GRAPH_H
#define SH_PACKAGE_LINK_GRAPH_H

#include <stddef.h>
#include <stdint.h>

typedef struct sh_package_link_edge {
    const char *slot;             /* unique native field/record identity */
    size_t target;                /* index in this resolved graph */
} sh_package_link_edge;

typedef struct sh_package_link_node {
    const char *family, *path;     /* native reader and original canonical path */
    uint64_t length;
    unsigned char content[32];    /* strong identity of the exact source bytes */
    unsigned char semantics[32];  /* additional bound presentation/reader state */
    const sh_package_link_edge *edges;
    size_t edge_count;
} sh_package_link_node;

typedef struct sh_package_link_classes {
    size_t *nodes;                /* graph node -> equivalence class */
    size_t *representatives;      /* equivalence class -> one graph node */
    unsigned char (*identities)[32]; /* stable binding identities, when requested */
    size_t node_count, count;
    size_t examined;              /* nodes examined during dependency refinement */
} sh_package_link_classes;

/* Certify a positive fingerprint match against retained exact source data and
 * reader semantics. Called only for equal local shapes, before graph collapse.
 * Return 1 only for exact equality; 0 aborts the candidate with a diagnostic.
 * A digest collision is not permission to share or a package authoring error. */
typedef int (*sh_package_link_verify_equal)(void *context, size_t left, size_t right,
    char *error, size_t capacity);

/* Every edge must already resolve in its authored scope. Missing dependencies,
 * unknown reader contracts and mutable native state are the caller's admission
 * responsibility, not equivalent empty graphs. Content identity alone never
 * certifies a shareable bound object. Package IDs, folder names and delivery
 * grouping deliberately do not enter this comparison.
 *
 * Compute the coarsest equal-binding partition, including cyclic graphs. Only
 * predecessors of a split class need reconsideration; no recursive hashes or
 * whole-graph rescans per dependency depth. Edge order is normalized by slot;
 * duplicate slots and out-of-range targets are rejected before partitioning.
 * Class numbers/representatives are local to this result, not persistent IDs.
 * The entry points without a verifier compare supplied fingerprints only;
 * callers must already have certified positive exact matches before using
 * their partitions to share objects. The verified forms perform that check
 * before any positive shape group collapses.
 * Inputs remain borrowed and unchanged. Initialize output to zero; repeated
 * calls replace it and failure clears it. */
int sh_package_link_equivalence(const sh_package_link_node *nodes, size_t count,
    sh_package_link_classes *out, char *error, size_t capacity);
int sh_package_link_equivalence_verified(const sh_package_link_node *nodes, size_t count,
    sh_package_link_verify_equal verify, void *context,
    sh_package_link_classes *out, char *error, size_t capacity);
void sh_package_link_classes_free(sh_package_link_classes *classes);

/* Also derive content-addressed identities over the resolved quotient graph.
 * Dependencies are hashed before their parents; a cyclic component gets one
 * canonical internal ordering. Adding/reordering unrelated packages or exact
 * duplicate resources does not rename existing assets. Source identity and
 * bound dependency identity remain separate from delivery identity. */
int sh_package_link_identify(const sh_package_link_node *nodes, size_t count,
    sh_package_link_classes *out, char *error, size_t capacity);
int sh_package_link_identify_verified(const sh_package_link_node *nodes, size_t count,
    sh_package_link_verify_equal verify, void *context,
    sh_package_link_classes *out, char *error, size_t capacity);

/* Retain an original native binding only when this node and every reachable
 * dependency are certified original. The caller certifies local bytes and
 * implicit reader semantics; this walk supplies the transitive explicit-edge
 * condition. Equal classes must receive the same conservative certification.
 * Cycles of originals remain original; one changed member invalidates every
 * predecessor. O(nodes + edges), without recursion. out has count bytes, may
 * equal local, and is cleared on failure. Inputs remain borrowed. */
int sh_package_link_originals(const sh_package_link_node *nodes, size_t count,
    const unsigned char *local, unsigned char *out, char *error, size_t capacity);

#endif
