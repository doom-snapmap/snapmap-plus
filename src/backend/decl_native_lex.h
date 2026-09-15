/* Source slices under the engine declaration parser's 0x4c430 lexer flags. */
#ifndef SH_DECL_NATIVE_LEX_H
#define SH_DECL_NATIVE_LEX_H
#include "decl_tree.h"

typedef enum sh_decl_token_kind {
    SH_DECL_TOKEN_NAME, SH_DECL_TOKEN_NUMBER, SH_DECL_TOKEN_STRING,
    SH_DECL_TOKEN_LITERAL, SH_DECL_TOKEN_PUNCTUATION
} sh_decl_token_kind;

typedef struct sh_decl_token {
    size_t begin, end, line, close;
    sh_decl_token_kind kind;
} sh_decl_token;

typedef struct sh_decl_tokens {
    sh_decl_source source; /* Borrowed until the token set is released. */
    sh_decl_token *items;
    size_t count;
} sh_decl_tokens;

/* No native resource loads, preprocessing or source rewriting. Preserve exact
 * spelling and line boundaries; pair ordinary delimiters with a heap stack.
 * Declaration flags disable string escapes/implicit string concatenation and
 * allow path characters in names. Explicit string continuation, preprocessor
 * directives, verbatim blocks and non-decimal numbers currently refuse rather
 * than acquiring guessed semantics. Failure leaves an empty output. */
int sh_decl_native_lex(sh_decl_source source, sh_decl_tokens *out,
    char *error, size_t capacity);
void sh_decl_tokens_free(sh_decl_tokens *tokens);
int sh_decl_token_is(const sh_decl_tokens *tokens, size_t index, const char *text);
#endif
