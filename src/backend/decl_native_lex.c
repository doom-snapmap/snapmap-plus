#include "decl_native_lex.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int nl_alpha(unsigned char ch)
{ return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_'; }
static int nl_digit(unsigned char ch)
{ return ch >= '0' && ch <= '9'; }
static int nl_space(unsigned char ch)
{ return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v'; }
static int nl_name(unsigned char ch)
{ return nl_alpha(ch) || nl_digit(ch) || (ch && strchr("/\\:.$", ch)); }

static int nl_fail(char *error, size_t capacity, size_t line, const char *reason)
{
    if (error && capacity) snprintf(error, capacity, "declaration lexer line %zu: %s", line, reason);
    return 0;
}
void sh_decl_tokens_free(sh_decl_tokens *tokens)
{
    if (tokens) { free(tokens->items); memset(tokens, 0, sizeof(*tokens)); }
}
int sh_decl_token_is(const sh_decl_tokens *tokens, size_t index, const char *text)
{
    return tokens && text && index < tokens->count &&
        tokens->items[index].end - tokens->items[index].begin == strlen(text) &&
        !memcmp(tokens->source.text + tokens->items[index].begin, text, strlen(text));
}

int sh_decl_native_lex(sh_decl_source source, sh_decl_tokens *out, char *error, size_t capacity)
{
    const char *s = source.text;
    size_t p = 0, line = 1, allocated = 0, *stack = NULL, depth = 0, stack_capacity = 0;
    const char *reason = "invalid source bytes";
    if (error && capacity) *error = 0;
    if (!out) return nl_fail(error, capacity, 1, "missing token destination");
    memset(out, 0, sizeof(*out));
    if (!s || memchr(s, 0, source.length)) goto bad;
    out->source = source;
    while (p < source.length) {
        size_t begin, token_line, i;
        unsigned char ch = (unsigned char)s[p];
        sh_decl_token_kind kind;
        if (nl_space(ch)) { line += ch == '\n'; p++; continue; }
        if (ch == '/' && p + 1 < source.length && s[p + 1] == '/') {
            p += 2; while (p < source.length && s[p] != '\n') p++; continue;
        }
        if (ch == '/' && p + 1 < source.length && s[p + 1] == '*') {
            int closed = 0;
            p += 2;
            while (p < source.length) {
                if (s[p] == '/' && p + 1 < source.length && s[p + 1] == '*') {
                    reason = "nested comment requires native diagnostic handling"; goto bad;
                }
                if (s[p] == '*' && p + 1 < source.length && s[p + 1] == '/') { p += 2; closed = 1; break; }
                line += s[p] == '\n'; p++;
            }
            if (!closed) { reason = "unterminated comment"; goto bad; }
            continue;
        }
        begin = p; token_line = line;
        if (ch == '"' || ch == '\'') {
            kind = ch == '"' ? SH_DECL_TOKEN_STRING : SH_DECL_TOKEN_LITERAL;
            p++;
            /* Under 0x20 a backslash is a byte, even before the closing quote. */
            while (p < source.length && s[p] != ch) {
                if ((unsigned char)s[p] < 32 && s[p] != '\t') { reason = "control byte inside quoted token"; goto bad; }
                p++;
            }
            if (p == source.length) { reason = "unterminated quoted token"; goto bad; }
            p++;
        } else if (nl_digit(ch) || (ch == '.' && p + 1 < source.length && nl_digit((unsigned char)s[p + 1]))) {
            kind = SH_DECL_TOKEN_NUMBER;
            if (ch == '0' && p + 1 < source.length && strchr("xXbB", s[p + 1])) {
                reason = "non-decimal number requires its native numeric reader"; goto bad;
            }
            while (p < source.length && nl_digit((unsigned char)s[p])) p++;
            if (p < source.length && s[p] == '.') {
                p++; while (p < source.length && nl_digit((unsigned char)s[p])) p++;
            }
            if (p < source.length && (s[p] == 'e' || s[p] == 'E')) {
                size_t exponent;
                p++; if (p < source.length && (s[p] == '+' || s[p] == '-')) p++;
                exponent = p; while (p < source.length && nl_digit((unsigned char)s[p])) p++;
                if (p == exponent) { reason = "missing numeric exponent"; goto bad; }
            }
            /* Native octal, float suffix and exceptional values need separate
             * conversion rules. Do not label them ordinary decimal literals. */
            if ((ch == '0' && p > begin + 1 && nl_digit((unsigned char)s[begin + 1])) ||
                (p < source.length && (nl_alpha((unsigned char)s[p]) || s[p] == '.' || s[p] == '#'))) {
                reason = "unsupported native numeric spelling"; goto bad;
            }
        } else if (nl_alpha(ch) || ch == '.') {
            kind = SH_DECL_TOKEN_NAME;
            p++; while (p < source.length && nl_name((unsigned char)s[p])) p++;
        } else {
            static const char *pairs[] = {">=", "<=", "==", "!=", "&&", "||", "++", "--", "+=", "-=", "*=", "/=", "%=", "<<", ">>", "&=", "|=", "^=", "::", "->"};
            kind = SH_DECL_TOKEN_PUNCTUATION;
            if (ch == '#' || ch == '$') { reason = "preprocessor input requires source expansion"; goto bad; }
            if (ch == '\\') { reason = "explicit string continuation requires its native reader"; goto bad; }
            if (ch == '<' && p + 1 < source.length && s[p + 1] == '%') { reason = "verbatim block requires its native reader"; goto bad; }
            if (!strchr("{}()[];,:?~!+-*/%=<>&|^", ch)) { reason = "unsupported punctuation or source byte"; goto bad; }
            p++;
            for (i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
                if (p < source.length && s[begin] == pairs[i][0] && s[p] == pairs[i][1]) { p++; break; }
            }
        }
        if (out->count == allocated) {
            size_t next = allocated ? allocated * 2 : 128;
            sh_decl_token *items = next > allocated && next <= SIZE_MAX / sizeof(*items) ?
                realloc(out->items, next * sizeof(*items)) : NULL;
            if (!items) { reason = "token allocation failed"; goto bad; }
            allocated = next; out->items = items;
        }
        i = out->count++;
        out->items[i] = (sh_decl_token){begin, p, token_line, SIZE_MAX, kind};
        if (kind != SH_DECL_TOKEN_PUNCTUATION || p != begin + 1) continue;
        if (strchr("{([", ch)) {
            if (depth == stack_capacity) {
                size_t next = stack_capacity ? stack_capacity * 2 : 16;
                size_t *grown = next > stack_capacity && next <= SIZE_MAX / sizeof(*grown) ?
                    realloc(stack, next * sizeof(*grown)) : NULL;
                if (!grown) { reason = "delimiter allocation failed"; goto bad; }
                stack = grown; stack_capacity = next;
            }
            stack[depth++] = i;
        } else if (strchr("})]", ch)) {
            char opening = ch == '}' ? '{' : ch == ')' ? '(' : '[';
            if (!depth || s[out->items[stack[depth - 1]].begin] != opening) { reason = "mismatched delimiter"; goto bad; }
            out->items[stack[--depth]].close = i;
        }
    }
    if (depth) { reason = "unclosed delimiter"; goto bad; }
    free(stack); return 1;
bad:
    free(stack); sh_decl_tokens_free(out); return nl_fail(error, capacity, line, reason);
}
