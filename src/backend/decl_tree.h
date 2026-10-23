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
/* 1 when siblings anywhere below node share a key (an ordered parse kept a
 * repeat), 0 when none do, -1 on allocation failure. */
int sh_decl_tree_repeats(const sh_decl_node *node);
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

/* entityDef Parse (VK 0x1417ad460 / GL 0x14179ff40) consumes its headers by
 * position: inherit, class, expandInheritance, poolCount, poolGranularity,
 * then the editorVars block (VK 0x1417ad960 / GL 0x1417a0440). Each is tried
 * once, in that order, with a case-sensitive CheckTokenString (VK 0x141a0f050
 * / GL 0x141a01280) that consumes only a match of the next token. The state
 * text starts at the first statement not consumed, so a repeated or
 * out-of-order header is state text, never a second header, and a statement
 * with type/ops tokens never matches. Consumed headers are therefore a prefix
 * of the definition's members. Fields borrow those members (NULL when absent);
 * state is the first state-text statement. Shape checks stay with callers. */
/* The native update-mode layer merge (VK 0x141a65820 / GL 0x141a57cf0 with
 * its flag set): each source member, in order, updates target's first
 * case-sensitively equal child, recursively for blocks, or is appended. The
 * source is consumed. 0 refuses (differently cased spellings or a block/scalar
 * mismatch) or fails allocation; target may then hold a partial merge. */
int sh_decl_tree_overlay(sh_decl_node *target, sh_decl_node *source);
/* Collapse a block's own repeated members by that merge, in place: the result
 * has one member per name, at its first position, holding the in-order merge
 * of every occurrence. Merging the result in update mode equals merging the
 * original (first-occurrence append order and last values are kept). It is not
 * equivalent where every occurrence is appended (a first stateful layer).
 * 1 done, 0 refused, -1 allocation failure. */
int sh_decl_tree_collapse(sh_decl_node *block);

typedef struct sh_decl_entity_headers {
    const sh_decl_node *inherit, *class_name, *expand, *pool_count, *pool_granularity, *editor_vars;
    const sh_decl_node *state;
} sh_decl_entity_headers;
void sh_decl_entity_split(const sh_decl_node *definition, sh_decl_entity_headers *headers);
/* The definition's own expansion mode, as entityDef Parse reads it (VK
 * 0x1417ad460 / GL 0x14179ff40): only the positional expandInheritance header
 * counts, nothing is inherited, and without it the constructor's 1 stays. Its
 * value is one ParseBool token (VK 0x141a14340 / GL 0x141a06580): the names
 * true or false, compared case-sensitively, or a number whose 32-bit integer
 * value (decimal, hex, binary or octal, wrapping at any length, or a float
 * truncated) is nonzero for true; then Parse expects ";". The mode selects only this
 * definition's own prepared text (VK 0x1417ae9d0 / GL 0x1417a14b0): 1 merges
 * the chain's own state slices root-first, 0 keeps its own state text alone.
 * An inheriting declaration's merge (VK 0x1417aec10 / GL 0x1417a16f0) reads
 * each ancestor's own slice and never its mode. -1: a value native Parse
 * refuses (a string, a sign, a second dot, a float exception, a token before
 * the ";"); -2: a spelling it would hand to the preprocessor (another name,
 * which may be a define, or # and $ input), which this reader does not model. */
int sh_decl_entity_expands(const sh_decl_node *definition);
/* The statement of an entityDef's own text that reaches its prepared first
 * statement, decided from the text alone. Without a positional parent (none,
 * or the empty one Parse reads without a lookup) it is the first state
 * statement, whatever its name: SetEntityEditState reads only that one and
 * checks no name, so later statements are inert. Under a parent it is the
 * first edit statement, which updates an edit-named inherited state by first
 * match; another inherited name needs the parent (sh_decl_entity_merges).
 * An unexpanded definition (sh_decl_entity_expands 0) prepares its own text
 * alone, so the parentless rule applies to it under a parent too.
 * NULL when absent; *parentless reports which rule applied. */
const sh_decl_node *sh_decl_entity_own_state(const sh_decl_node *definition, int *parentless);
/* The same selection with the inheritance known (sh_decl_entity_merges):
 * state "" for a definition without a stateful ancestor selects its first
 * state statement; a name selects the first own statement of that name, and
 * *update is set because every later one of that name updates it in order
 * (sh_decl_entity_next_state). NULL state decides from the text alone. */
const sh_decl_node *sh_decl_entity_state_statement(const sh_decl_node *definition, const char *state, int *update);
const sh_decl_node *sh_decl_entity_next_state(const sh_decl_node *statement);
/* The effective state text of a definition in update mode (state a name, as
 * sh_decl_entity_merges resolves it). Every statement after the headers merges
 * in order by first match into the inherited tree, so collapsing that text
 * (sh_decl_tree_collapse, which merges blocks recursively) is the same merge
 * with one statement and one member per name, each at its first position: the
 * effective rows a single-statement reader sees. A first stateful
 * layer ("" or NULL) is left as authored, since there every occurrence is
 * appended. 1 done, 0 refused (mixed spellings or value kinds), -1 allocation
 * failure; after 0 or -1 the definition is partially merged and is discarded. */
int sh_decl_entity_state_collapse(sh_decl_node *definition, const char *state);

#endif
