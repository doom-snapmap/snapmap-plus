/* decl_text.h -- bounded structural validation shared by decl file consumers. */
#ifndef BACKEND_DECL_TEXT_H
#define BACKEND_DECL_TEXT_H

#include <stddef.h>

/* Return 1 when `text` is non-empty, contains no embedded NUL, and has balanced
 * braces, quotes, and comments. This is deliberately structural only; DOOM's
 * own decl parser remains the semantic authority. */
int sh_decl_text_well_formed(const unsigned char *text, size_t length);

/* A placed entity's own decl text from its prepared text (`prepared`, the
 * inherit chain merged root first with its own members) and its inherit's
 * prepared text (`inherited`; may be empty). Every leaf whose key path and
 * value equal the inherit's is dropped, as the engine drops them when it
 * reads a saved entity, and so is a block left empty; the first statement is
 * always written. The result is tab-indented `key = value;` lines. Returns 0,
 * with `out` empty, for a malformed or truncated text or a full `out`. */
int sh_decl_text_own_state(const unsigned char *prepared, size_t prepared_length,
                           const unsigned char *inherited, size_t inherited_length,
                           char *out, size_t capacity, size_t *out_length);

#endif /* BACKEND_DECL_TEXT_H */
