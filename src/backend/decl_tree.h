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

/* Serialize an owned tree without recursion or an application payload cap.
 * Scalar spelling and sibling order are retained. The result is malloc-owned. */
char *sh_decl_tree_write(const sh_decl_node *root, size_t *length);
void sh_decl_tree_free(sh_decl_node *node);
const sh_decl_node *sh_decl_tree_member(const sh_decl_node *parent, const char *key);

/* Borrow one plain token or quoted string, without its quotes. Escape sequences
 * and expressions return zero: the owning resource reader must interpret those
 * with its native lexer settings, rather than guessing a resource name. */
int sh_decl_tree_literal(const sh_decl_node *node, const char **text, size_t *length);

#endif
