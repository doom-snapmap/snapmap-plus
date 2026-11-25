#pragma once
#include "../common/text.h"

enum { MD6_END, MD6_WORD, MD6_STR, MD6_LB, MD6_RB, MD6_OTHER };
typedef struct md6_token { int k; const char *p; size_t n; } md6_token;

/* Lexes a decl text as the engine does: whitespace and comments skipped, quoted text one token. */
static __inline md6_token md6_next(const char *t, size_t len, size_t *pos)
{
    md6_token k = { MD6_END, NULL, 0 };
    size_t i = *pos;
    for (;;) {
        while (i < len && (t[i] == ' ' || t[i] == '\t' || t[i] == '\r' || t[i] == '\n')) i++;
        if (i + 1 < len && t[i] == '/' && t[i + 1] == '/') { while (i < len && t[i] != '\n') i++; continue; }
        if (i + 1 < len && t[i] == '/' && t[i + 1] == '*') {
            i += 2;
            while (i + 1 < len && !(t[i] == '*' && t[i + 1] == '/')) i++;
            i = i + 2 <= len ? i + 2 : len;
            continue;
        }
        break;
    }
    if (i >= len) { *pos = len; return k; }
    if (t[i] == '"') {
        size_t s = ++i;
        while (i < len && t[i] != '"' && t[i] != '\n') i++;
        k.k = MD6_STR; k.p = t + s; k.n = i - s;
        if (i < len && t[i] == '"') i++;
    } else if (t[i] == '{') { k.k = MD6_LB; i++; }
    else if (t[i] == '}') { k.k = MD6_RB; i++; }
    else if ((t[i] >= 'a' && t[i] <= 'z') || (t[i] >= 'A' && t[i] <= 'Z') || t[i] == '_') {
        size_t s = i;
        while (i < len && ((t[i] >= 'a' && t[i] <= 'z') || (t[i] >= 'A' && t[i] <= 'Z') ||
                           (t[i] >= '0' && t[i] <= '9') || t[i] == '_')) i++;
        k.k = MD6_WORD; k.p = t + s; k.n = i - s;
    } else { k.k = MD6_OTHER; i++; }
    *pos = i;
    return k;
}

static __inline int md6_word(const md6_token *t, const char *w)
{
    size_t n = strlen(w), i;
    if (t->k != MD6_WORD || t->n != n) return 0;
    for (i = 0; i < n; i++) if (sh_path_fold(t->p[i]) != w[i]) return 0;
    return 1;
}

/* Writes a JSON string, escaping quote and backslash. */
static __inline void md6_put_name(sh_text_buf *b, const char *s)
{
    sh_text_append(b, "\"", 1);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') sh_text_append(b, "\\", 1);
        if ((unsigned char)*s >= 0x20) sh_text_append(b, s, 1);
    }
    sh_text_append(b, "\"", 1);
}
