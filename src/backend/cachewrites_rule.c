/* cachewrites_rule.c -- which of the engine's writes is a cache write, and where it goes (cachewrites.h). Pure. */
#include <string.h>

#include "cachewrites.h"

static int is_sep(char c) { return c == '/' || c == '\\'; }

static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c; }

/* Tests that rel begins with `word` followed by a slash, ASCII case-insensitively. */
static int begins_dir(const char *rel, const char *word)
{
    size_t i;
    for (i = 0; word[i]; i++)
        if (lower(rel[i]) != word[i]) return 0;
    return is_sep(rel[i]);
}

static int ends_with(const char *rel, size_t n, const char *tail)
{
    size_t t = strlen(tail), i;
    if (n < t) return 0;
    for (i = 0; i < t; i++)
        if (lower(rel[n - t + i]) != tail[i]) return 0;
    return 1;
}

/* Tests that rel has a `..` element between slashes or at either end. */
static int has_parent(const char *rel)
{
    const char *p = rel;
    while (*p) {
        const char *e = p;
        while (*e && !is_sep(*e)) e++;
        if (e - p == 2 && p[0] == '.' && p[1] == '.') return 1;
        p = *e ? e + 1 : e;
    }
    return 0;
}

int sh_cachewrite_matches(const char *rel)
{
    if (!rel || !*rel || has_parent(rel)) return 0;
    return begins_dir(rel, "generated") || (begins_dir(rel, "maps") && ends_with(rel, strlen(rel), ".flight"));
}

int sh_cachewrite_redirect(const char *rel, const char *root, char *out, size_t cap)
{
    static const char BASE[] = "\\base\\";
    size_t rn, ln, i, at;
    if (!root || !*root || !out || !sh_cachewrite_matches(rel)) return 0;
    ln = strlen(rel);
    rn = strlen(root);
    while (rn && is_sep(root[rn - 1])) rn--;
    if (!rn) return 0;
    if (rn + (sizeof BASE - 1) + ln + 1 > cap) return 0;
    memcpy(out, root, rn);
    at = rn;
    memcpy(out + at, BASE, sizeof BASE - 1);
    at += sizeof BASE - 1;
    for (i = 0; i < ln; i++) out[at + i] = rel[i] == '/' ? '\\' : rel[i];
    out[at + ln] = '\0';
    return 1;
}
