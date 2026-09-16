/* Composition of registered native block-grammar declaration families.
 *
 * Several engine families have a custom reader rather than reflected state, so
 * the generic reflected composer cannot describe them. Their readers share one
 * shape: a braced body of records, where a record is either a keyword followed
 * by that keyword's values, or a keyword (optionally carrying a name) that
 * opens a nested body. This adapter drives that shared shape from a per-family
 * table of the keywords each native reader accepts, so the record boundary is
 * the native field boundary and never a source line break.
 *
 * Record values are retained as the author's own bytes and re-emitted
 * unchanged; only the whitespace between records is regenerated. A body whose
 * native grammar is not established composes as one indivisible record, which
 * keeps whole-payload selection as the boundary instead of guessing a field.
 */
#ifndef SH_DECL_BLOCK_COMPOSE_H
#define SH_DECL_BLOCK_COMPOSE_H
#include "decl_compose.h"

/* 1 when this declaration family has a verified block grammar. */
int sh_decl_block_family(const char *family);

/* Compose one native block-grammar declaration. The caller establishes the
 * family and the baseline's provenance. Independent record changes merge;
 * two contributions that change the same record differently refuse with that
 * record's path. Unsupported syntax refuses rather than composing a guess.
 * Successful output is malloc-owned; inputs are never modified. */
char *sh_decl_block_compose(const char *family, sh_decl_source baseline,
    const sh_decl_source *sources, size_t count, size_t *length,
    char *error, size_t capacity, sh_decl_conflict *conflict);

#endif
