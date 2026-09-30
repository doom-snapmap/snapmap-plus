/* Read-only syntax trees for assignment-style native declarations. */
#ifndef SH_DECL_TREE_H
#define SH_DECL_TREE_H

#include <stddef.h>

typedef struct sh_decl_source {
    const char *text;
    size_t length;
} sh_decl_source;

/* Values retain their original scalar spelling. Keys and values are owned by
 * the tree; callers must not modify them while a traversal is in progress. */
typedef struct sh_decl_node {
    char *key, *value;
    struct sh_decl_node *children, *next;
    int compound, reset, assignment;
    /* Native type/ops tokens before an assignment key. Retained verbatim;
     * reference binding still uses the registered field schema. */
    char *annotation;
} sh_decl_node;

/* Parse a complete braced declaration. Unsupported syntax, duplicate keys,
 * embedded NUL and allocation failure return NULL with a diagnostic. Parsing
 * and destruction use heap storage rather than recursion or a payload cap. */
sh_decl_node *sh_decl_tree_parse(sh_decl_source source, char *error, size_t error_capacity);

/* Native custom readers may use repeated fields as ordered records (graph
 * nodes, links and layers). Retain every occurrence without assigning merge
 * semantics. The owning adapter must validate its complete grammar before
 * lookup or composition. The ordinary parser above continues to reject repeats. */
sh_decl_node *sh_decl_tree_parse_ordered(sh_decl_source source, char *error, size_t error_capacity);

/* The shared reflected declaration reader collects a body as a braced section
 * and ignores that section's end-of-file result, so end of file may close the
 * outermost brace. Nested blocks must still close. Use only for a family whose
 * native Parse is verified to end in that reader. Ordered retains repeats. */
sh_decl_node *sh_decl_tree_parse_reflected_body(sh_decl_source source, int ordered,
    char *error, size_t error_capacity);

/* Diagnostic parse census of the calling thread: parses and text bytes.
 * Never used for decisions. open returns 0 when this thread already has one
 * (the caller then only reads it) or allocation fails; read yields zeros
 * without a census. */
typedef struct sh_decl_tree_census {
    unsigned long long parses, bytes;
} sh_decl_tree_census;
int sh_decl_tree_census_open(void);
void sh_decl_tree_census_read(sh_decl_tree_census *out);
void sh_decl_tree_census_close(void);

/* Serialize an owned tree without recursion or an application payload cap.
 * Scalar spelling and sibling order are retained. The result is malloc-owned. */
char *sh_decl_tree_write(const sh_decl_node *root, size_t *length);
void sh_decl_tree_free(sh_decl_node *node);
const sh_decl_node *sh_decl_tree_member(const sh_decl_node *parent, const char *key);
/* Structural equality under the native parser's in-order application:
 * members compare in sequence (list counts, resets and repeated overlays apply
 * in order), with keys, scalar spelling, resets, block kinds, `key = {` versus
 * `key {` and type annotations exact. unordered (may be NULL) names top-level
 * blocks, separated by '|', whose direct members may appear in any order, each
 * still compared in sequence below it; use it only where reordering is proven
 * harmless. 1 equal, 0 different, -1 on allocation failure. */
int sh_decl_tree_equal(const sh_decl_node *a, const sh_decl_node *b, const char *unordered);

/* Borrow one plain token or quoted string, without its quotes. Declaration
 * lexer flags disable escapes, so backslashes remain literal path bytes.
 * Expressions and multiple quoted tokens return zero. JSON has its own reader. */
int sh_decl_tree_literal(const sh_decl_node *node, const char **text, size_t *length);

/* Native idList sizes use a signed 32-bit count; non-positive sizes clear the
 * list. Decode decimal scalar counts without changing authored spelling. */
int sh_decl_tree_list_count(const sh_decl_node *node, size_t *count);

#endif
