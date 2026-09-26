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

/* One subweb a contribution now owns under its own name (see
 * sh_decl_animweb_prepare). from and to are malloc-owned. */
typedef struct sh_decl_animweb_move { size_t source; char *from, *to; } sh_decl_animweb_move;

/* Align whole-copy contributions of the player's hands web with its stock
 * original before they compose. The web is one global registry: every weapon
 * names its subweb and states in it, and edges connect subwebs by name.
 * - modelInfos becomes the stock list followed by each contribution's new
 *   models in order, and every modelIndex follows its model;
 * - editor view settings keep their stock values;
 * - an edge a contribution adds into a subweb the stock web lacks is glue: it
 *   joins the subweb it is written in;
 * - a stock subweb a contribution changes otherwise, or a new subweb an
 *   earlier contribution defines differently, becomes that contribution's own
 *   copy named <name>_<content hash>: its own edges follow it, each of the
 *   contribution's edges entering it gains a twin entering the copy, the
 *   stock subweb (with the contribution's glue) stays, and equal copies share
 *   one name;
 * - a contribution never deletes a stock subweb or state.
 * sources are taken in the caller's stable order and replaced in place by
 * rewritten texts, which owned[i] (count entries) then holds; moves lists each
 * copy for the caller's weapon bindings. */
int sh_decl_animweb_prepare(sh_decl_source baseline, sh_decl_source *sources, size_t count,
    char **owned, sh_decl_animweb_move **moves, size_t *move_count, char *error, size_t capacity);
void sh_decl_animweb_moves_free(sh_decl_animweb_move *moves, size_t count);

/* A weapon declaration whose subweb_normal or handsHitReactionSubweb names
 * subweb `from` names `to`, the copy its package now owns. NULL with *changed
 * 0 when it names neither (or on failure, with error set). */
char *sh_decl_weapon_subweb_rename(sh_decl_source weapon, const char *from, const char *to,
    size_t *length, int *changed, char *error, size_t capacity);

#endif
